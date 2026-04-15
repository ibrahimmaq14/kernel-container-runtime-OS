/*
 * OS-Jackfruit user-space runtime
 * Authors:
 *   Mohammed Ibrahim Maqsood Khan (PES1UG24C578)
 *   Nitesh Harsur (PES1UG24CS584)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>

#include "../include/osj_uapi.h"

#define OSJ_MAX_CONTAINERS 64
#define OSJ_MAX_IO_SOURCES (OSJ_MAX_CONTAINERS * 2)
#define OSJ_LOG_QUEUE_CAPACITY 1024
#define OSJ_LOG_PAYLOAD_MAX 256
#define OSJ_LINE_MAX 1024
#define OSJ_MONITOR_DEVICE "/dev/osj_monitor"
#define OSJ_LOG_DIR "logs"
#define OSJ_STOP_WAIT_MS 3000

enum container_state {
	CONTAINER_EMPTY = 0,
	CONTAINER_CREATED,
	CONTAINER_STARTING,
	CONTAINER_RUNNING,
	CONTAINER_STOPPING,
	CONTAINER_EXITED,
	CONTAINER_FAILED,
};

enum log_stream_type {
	LOG_STREAM_ENGINE = 0,
	LOG_STREAM_STDOUT,
	LOG_STREAM_STDERR,
};

struct log_record {
	uint32_t container_id;
	enum log_stream_type stream;
	struct timespec ts;
	size_t len;
	char payload[OSJ_LOG_PAYLOAD_MAX];
};

struct log_queue {
	struct log_record records[OSJ_LOG_QUEUE_CAPACITY];
	size_t head;
	size_t tail;
	size_t count;
	bool closing;
	pthread_mutex_t lock;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
};

struct container {
	bool used;
	bool registered_in_kernel;
	bool stop_requested;
	bool stdout_closed;
	bool stderr_closed;
	uint32_t id;
	enum container_state state;
	pid_t launcher_pid;
	pid_t init_pid;
	int exit_code;
	int term_signal;
	uint64_t soft_limit_bytes;
	uint64_t hard_limit_bytes;
	uint64_t last_rss_bytes;
	uint64_t peak_rss_bytes;
	uint32_t member_count;
	int stdout_fd;
	int stderr_fd;
	int log_fd;
	char name[OSJ_NAME_LEN];
	char command_line[PATH_MAX];
	char log_path[PATH_MAX];
	struct timespec started_at;
	struct timespec exited_at;
};

struct io_source {
	bool active;
	int fd;
	uint32_t container_id;
	enum log_stream_type stream;
};

struct pending_exit {
	bool used;
	pid_t pid;
	int status;
};

struct runtime {
	pthread_mutex_t containers_lock;
	struct container containers[OSJ_MAX_CONTAINERS];
	struct io_source io_sources[OSJ_MAX_IO_SOURCES];
	struct pending_exit pending_exits[OSJ_MAX_CONTAINERS];
	uint32_t next_container_id;
	int epoll_fd;
	int monitor_fd;
	atomic_bool shutdown_requested;
	atomic_bool io_thread_ready;
	struct log_queue log_queue;
	pthread_t logger_thread;
	pthread_t io_thread;
	pthread_t signal_thread;
	sigset_t signal_set;
};

struct start_request {
	const char *name;
	uint64_t soft_limit_bytes;
	uint64_t hard_limit_bytes;
	char **argv;
	const char *command_line;
};

struct child_exec_params {
	const struct start_request *req;
	int stdout_write_fd;
	int stderr_write_fd;
};

static const char *container_state_name(enum container_state state)
{
	switch (state) {
	case CONTAINER_CREATED:
		return "created";
	case CONTAINER_STARTING:
		return "starting";
	case CONTAINER_RUNNING:
		return "running";
	case CONTAINER_STOPPING:
		return "stopping";
	case CONTAINER_EXITED:
		return "exited";
	case CONTAINER_FAILED:
		return "failed";
	case CONTAINER_EMPTY:
	default:
		return "empty";
	}
}

static const char *log_stream_name(enum log_stream_type stream)
{
	switch (stream) {
	case LOG_STREAM_STDOUT:
		return "stdout";
	case LOG_STREAM_STDERR:
		return "stderr";
	case LOG_STREAM_ENGINE:
	default:
		return "engine";
	}
}

static int mkdir_if_missing(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 0;

	perror("mkdir");
	return -1;
}

static int set_nonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;

	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	return 0;
}

static void log_queue_init(struct log_queue *queue)
{
	memset(queue, 0, sizeof(*queue));
	pthread_mutex_init(&queue->lock, NULL);
	pthread_cond_init(&queue->not_empty, NULL);
	pthread_cond_init(&queue->not_full, NULL);
}

static void log_queue_close(struct log_queue *queue)
{
	pthread_mutex_lock(&queue->lock);
	queue->closing = true;
	pthread_cond_broadcast(&queue->not_empty);
	pthread_cond_broadcast(&queue->not_full);
	pthread_mutex_unlock(&queue->lock);
}

static int log_queue_push(struct log_queue *queue, const struct log_record *record)
{
	pthread_mutex_lock(&queue->lock);
	while (!queue->closing && queue->count == OSJ_LOG_QUEUE_CAPACITY)
		pthread_cond_wait(&queue->not_full, &queue->lock);

	if (queue->closing) {
		pthread_mutex_unlock(&queue->lock);
		return -1;
	}

	queue->records[queue->tail] = *record;
	queue->tail = (queue->tail + 1) % OSJ_LOG_QUEUE_CAPACITY;
	queue->count++;
	pthread_cond_signal(&queue->not_empty);
	pthread_mutex_unlock(&queue->lock);
	return 0;
}

static int log_queue_pop(struct log_queue *queue, struct log_record *record)
{
	pthread_mutex_lock(&queue->lock);
	while (!queue->closing && queue->count == 0)
		pthread_cond_wait(&queue->not_empty, &queue->lock);

	if (queue->count == 0) {
		pthread_mutex_unlock(&queue->lock);
		return -1;
	}

	*record = queue->records[queue->head];
	queue->head = (queue->head + 1) % OSJ_LOG_QUEUE_CAPACITY;
	queue->count--;
	pthread_cond_signal(&queue->not_full);
	pthread_mutex_unlock(&queue->lock);
	return 0;
}

static void push_engine_log(struct runtime *rt, uint32_t container_id,
			    const char *fmt, ...)
{
	struct log_record record;
	va_list ap;
	int written;

	memset(&record, 0, sizeof(record));
	record.container_id = container_id;
	record.stream = LOG_STREAM_ENGINE;
	clock_gettime(CLOCK_REALTIME, &record.ts);

	va_start(ap, fmt);
	written = vsnprintf(record.payload, sizeof(record.payload), fmt, ap);
	va_end(ap);
	if (written < 0)
		return;

	record.len = (size_t)written;
	if (record.len >= sizeof(record.payload))
		record.len = sizeof(record.payload) - 1;

	log_queue_push(&rt->log_queue, &record);
}

static struct container *find_container_by_id_locked(struct runtime *rt,
						      uint32_t id)
{
	size_t i;

	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		if (rt->containers[i].used && rt->containers[i].id == id)
			return &rt->containers[i];
	}

	return NULL;
}

static struct container *find_container_by_name_locked(struct runtime *rt,
							const char *name)
{
	size_t i;

	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		if (rt->containers[i].used &&
		    strncmp(rt->containers[i].name, name,
			    sizeof(rt->containers[i].name)) == 0)
			return &rt->containers[i];
	}

	return NULL;
}

static struct container *find_container_by_pid_locked(struct runtime *rt, pid_t pid)
{
	size_t i;

	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		if (!rt->containers[i].used)
			continue;

		if (rt->containers[i].launcher_pid == pid ||
		    rt->containers[i].init_pid == pid)
			return &rt->containers[i];
	}

	return NULL;
}

static void format_timestamp(const struct timespec *ts, char *buf, size_t buf_len)
{
	struct tm tm_buf;

	localtime_r(&ts->tv_sec, &tm_buf);
	strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &tm_buf);
}

static void *logger_thread_main(void *arg)
{
	struct runtime *rt = arg;
	struct log_record record;

	while (log_queue_pop(&rt->log_queue, &record) == 0) {
		struct container *container = NULL;
		char ts_buf[64];
		char line[512];
		ssize_t ignored;
		int fd = STDERR_FILENO;
		int len;

		pthread_mutex_lock(&rt->containers_lock);
		container = find_container_by_id_locked(rt, record.container_id);
		if (container && container->log_fd >= 0)
			fd = container->log_fd;
		pthread_mutex_unlock(&rt->containers_lock);

		format_timestamp(&record.ts, ts_buf, sizeof(ts_buf));
		len = snprintf(line, sizeof(line), "[%s.%03ld] [container:%u] [%s] %.*s\n",
			       ts_buf, record.ts.tv_nsec / 1000000L,
			       record.container_id, log_stream_name(record.stream),
			       (int)record.len, record.payload);
		if (len < 0)
			continue;

		if (len > (int)sizeof(line))
			len = sizeof(line);

		ignored = write(fd, line, (size_t)len);
		(void)ignored;
	}

	return NULL;
}

static int register_io_source(struct runtime *rt, int fd, uint32_t container_id,
			      enum log_stream_type stream)
{
	struct epoll_event ev;
	size_t i;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP;
	ev.data.fd = fd;

	if (epoll_ctl(rt->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		perror("epoll_ctl add");
		return -1;
	}

	pthread_mutex_lock(&rt->containers_lock);
	for (i = 0; i < OSJ_MAX_IO_SOURCES; i++) {
		if (!rt->io_sources[i].active) {
			rt->io_sources[i].active = true;
			rt->io_sources[i].fd = fd;
			rt->io_sources[i].container_id = container_id;
			rt->io_sources[i].stream = stream;
			pthread_mutex_unlock(&rt->containers_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&rt->containers_lock);

	epoll_ctl(rt->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
	errno = ENOSPC;
	perror("register_io_source");
	return -1;
}

static struct io_source *find_io_source_locked(struct runtime *rt, int fd)
{
	size_t i;

	for (i = 0; i < OSJ_MAX_IO_SOURCES; i++) {
		if (rt->io_sources[i].active && rt->io_sources[i].fd == fd)
			return &rt->io_sources[i];
	}

	return NULL;
}

static void mark_stream_closed_locked(struct runtime *rt, struct io_source *source)
{
	struct container *container;

	container = find_container_by_id_locked(rt, source->container_id);
	if (!container)
		return;

	if (source->stream == LOG_STREAM_STDOUT) {
		container->stdout_closed = true;
		container->stdout_fd = -1;
	} else if (source->stream == LOG_STREAM_STDERR) {
		container->stderr_closed = true;
		container->stderr_fd = -1;
	}
}

static void remove_io_source(struct runtime *rt, int fd)
{
	struct io_source *source;

	epoll_ctl(rt->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
	pthread_mutex_lock(&rt->containers_lock);
	source = find_io_source_locked(rt, fd);
	if (source) {
		mark_stream_closed_locked(rt, source);
		source->active = false;
		source->fd = -1;
	}
	pthread_mutex_unlock(&rt->containers_lock);
	close(fd);
}

static bool io_source_active(struct runtime *rt, int fd)
{
	bool active = false;

	pthread_mutex_lock(&rt->containers_lock);
	active = find_io_source_locked(rt, fd) != NULL;
	pthread_mutex_unlock(&rt->containers_lock);
	return active;
}

static void push_stream_log(struct runtime *rt, uint32_t container_id,
			    enum log_stream_type stream, const char *buf,
			    size_t len)
{
	size_t offset = 0;

	while (offset < len) {
		struct log_record record;
		size_t chunk = len - offset;

		if (chunk >= OSJ_LOG_PAYLOAD_MAX)
			chunk = OSJ_LOG_PAYLOAD_MAX - 1;

		memset(&record, 0, sizeof(record));
		record.container_id = container_id;
		record.stream = stream;
		clock_gettime(CLOCK_REALTIME, &record.ts);
		memcpy(record.payload, buf + offset, chunk);
		record.payload[chunk] = '\0';
		record.len = chunk;

		log_queue_push(&rt->log_queue, &record);
		offset += chunk;
	}
}

static void *io_thread_main(void *arg)
{
	struct runtime *rt = arg;
	struct epoll_event events[16];

	atomic_store(&rt->io_thread_ready, true);

	while (!atomic_load(&rt->shutdown_requested)) {
		int n;
		int i;

		n = epoll_wait(rt->epoll_fd, events, 16, 500);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			break;
		}

		for (i = 0; i < n; i++) {
			int fd = events[i].data.fd;
			char buffer[512];
			ssize_t bytes;
			uint32_t container_id = 0;
			enum log_stream_type stream = LOG_STREAM_STDOUT;

			pthread_mutex_lock(&rt->containers_lock);
			struct io_source *source = find_io_source_locked(rt, fd);
			if (source) {
				container_id = source->container_id;
				stream = source->stream;
			}
			pthread_mutex_unlock(&rt->containers_lock);

			if (!container_id)
				continue;

			while ((bytes = read(fd, buffer, sizeof(buffer))) > 0)
				push_stream_log(rt, container_id, stream, buffer,
						(size_t)bytes);

			if (bytes == 0 ||
			    (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
				remove_io_source(rt, fd);
		}
	}

	return NULL;
}

static int monitor_open(void)
{
	int fd = open(OSJ_MONITOR_DEVICE, O_RDWR | O_CLOEXEC);

	if (fd < 0)
		fprintf(stderr,
			"warning: unable to open %s (%s); containers will run without kernel memory enforcement\n",
			OSJ_MONITOR_DEVICE, strerror(errno));

	return fd;
}

static int monitor_register_container(struct runtime *rt,
				      const struct container *container)
{
	struct osj_container_register_req req;

	if (rt->monitor_fd < 0)
		return 0;

	memset(&req, 0, sizeof(req));
	req.abi_version = OSJ_ABI_VERSION;
	req.container_id = container->id;
	req.init_pid = container->init_pid;
	req.soft_limit_bytes = container->soft_limit_bytes;
	req.hard_limit_bytes = container->hard_limit_bytes;
	strncpy(req.name, container->name, sizeof(req.name) - 1);

	if (ioctl(rt->monitor_fd, OSJ_IOC_REGISTER_CONTAINER, &req) < 0) {
		perror("ioctl register");
		return -1;
	}

	return 0;
}

static int monitor_unregister_container(struct runtime *rt, uint32_t container_id)
{
	struct osj_container_unregister_req req;

	if (rt->monitor_fd < 0)
		return 0;

	memset(&req, 0, sizeof(req));
	req.abi_version = OSJ_ABI_VERSION;
	req.container_id = container_id;

	if (ioctl(rt->monitor_fd, OSJ_IOC_UNREGISTER_CONTAINER, &req) < 0) {
		perror("ioctl unregister");
		return -1;
	}

	return 0;
}

static int monitor_query_container(struct runtime *rt, struct container *container)
{
	struct osj_container_query_req req;

	if (rt->monitor_fd < 0)
		return 0;

	memset(&req, 0, sizeof(req));
	req.abi_version = OSJ_ABI_VERSION;
	req.container_id = container->id;

	if (ioctl(rt->monitor_fd, OSJ_IOC_QUERY_CONTAINER, &req) < 0) {
		if (errno != ENOENT)
			perror("ioctl query");
		return -1;
	}

	container->last_rss_bytes = req.current_rss_bytes;
	container->peak_rss_bytes = req.peak_rss_bytes;
	container->member_count = req.member_count;
	return 0;
}

static void close_container_fds(struct container *container)
{
	if (container->stdout_fd >= 0) {
		close(container->stdout_fd);
		container->stdout_fd = -1;
	}

	if (container->stderr_fd >= 0) {
		close(container->stderr_fd);
		container->stderr_fd = -1;
	}

	if (container->log_fd >= 0) {
		close(container->log_fd);
		container->log_fd = -1;
	}
}

static void update_container_exit_locked(struct runtime *rt, struct container *container,
					 int status)
{
	clock_gettime(CLOCK_REALTIME, &container->exited_at);

	if (WIFEXITED(status)) {
		container->exit_code = WEXITSTATUS(status);
		container->term_signal = 0;
		container->state = container->stop_requested ?
			CONTAINER_EXITED :
			(container->exit_code == 0 ? CONTAINER_EXITED : CONTAINER_FAILED);
	} else if (WIFSIGNALED(status)) {
		container->exit_code = -1;
		container->term_signal = WTERMSIG(status);
		container->state = CONTAINER_EXITED;
	} else {
		container->state = CONTAINER_FAILED;
	}

	if (container->registered_in_kernel) {
		monitor_unregister_container(rt, container->id);
		container->registered_in_kernel = false;
	}
}

static void save_pending_exit_locked(struct runtime *rt, pid_t pid, int status)
{
	size_t i;

	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		if (!rt->pending_exits[i].used) {
			rt->pending_exits[i].used = true;
			rt->pending_exits[i].pid = pid;
			rt->pending_exits[i].status = status;
			return;
		}
	}
}

static bool apply_pending_exit_locked(struct runtime *rt,
				      struct container *container,
				      int *status_out)
{
	size_t i;

	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		if (!rt->pending_exits[i].used)
			continue;
		if (rt->pending_exits[i].pid != container->launcher_pid)
			continue;

		update_container_exit_locked(rt, container,
					     rt->pending_exits[i].status);
		if (status_out)
			*status_out = rt->pending_exits[i].status;
		rt->pending_exits[i].used = false;
		return true;
	}

	return false;
}

static void reap_children(struct runtime *rt)
{
	for (;;) {
		int status;
		pid_t pid;
		uint32_t log_container_id = 0;
		pid_t log_launcher_pid = 0;
		pid_t log_init_pid = 0;
		char log_name[OSJ_NAME_LEN];

		pid = waitpid(-1, &status, WNOHANG);
		if (pid <= 0)
			break;

		pthread_mutex_lock(&rt->containers_lock);
		struct container *container = find_container_by_pid_locked(rt, pid);
		if (container) {
			update_container_exit_locked(rt, container, status);
			log_container_id = container->id;
			log_launcher_pid = container->launcher_pid;
			log_init_pid = container->init_pid;
			snprintf(log_name, sizeof(log_name), "%s", container->name);
		} else {
			save_pending_exit_locked(rt, pid, status);
		}
		pthread_mutex_unlock(&rt->containers_lock);

		if (log_container_id) {
			push_engine_log(rt, log_container_id,
					"container %s exited (launcher=%d init=%d status=0x%x)",
					log_name, log_launcher_pid, log_init_pid,
					status);
		}
	}
}

static void request_shutdown(struct runtime *rt)
{
	if (!atomic_exchange(&rt->shutdown_requested, true))
		push_engine_log(rt, 0, "runtime shutdown requested");
}

static void *signal_thread_main(void *arg)
{
	struct runtime *rt = arg;

	for (;;) {
		siginfo_t info;
		int sig;

		sig = sigwaitinfo(&rt->signal_set, &info);
		if (sig < 0) {
			if (errno == EINTR)
				continue;
			perror("sigwaitinfo");
			break;
		}

		if (sig == SIGCHLD) {
			reap_children(rt);
		} else if (sig == SIGINT || sig == SIGTERM) {
			request_shutdown(rt);
			break;
		}
	}

	return NULL;
}

static int parse_size_mb(const char *text, uint64_t *bytes_out)
{
	char *end = NULL;
	unsigned long long mb;

	errno = 0;
	mb = strtoull(text, &end, 10);
	if (errno != 0 || !end || *end != '\0')
		return -1;

	*bytes_out = mb * 1024ULL * 1024ULL;
	return 0;
}

static int parse_start_request(const char *line, struct start_request *req,
			       wordexp_t *expanded)
{
	const char *separator;

	memset(req, 0, sizeof(*req));
	memset(expanded, 0, sizeof(*expanded));

	if (wordexp(line, expanded, WRDE_NOCMD) != 0) {
		fprintf(stderr, "failed to parse command line\n");
		return -1;
	}

	if (expanded->we_wordc < 6 ||
	    strcmp(expanded->we_wordv[0], "start") != 0) {
		fprintf(stderr,
			"usage: start <name> <soft-limit-mb> <hard-limit-mb> -- <command> [args...]\n");
		wordfree(expanded);
		return -1;
	}

	if (strcmp(expanded->we_wordv[4], "--") != 0) {
		fprintf(stderr,
			"usage: start <name> <soft-limit-mb> <hard-limit-mb> -- <command> [args...]\n");
		wordfree(expanded);
		return -1;
	}

	req->name = expanded->we_wordv[1];
	if (parse_size_mb(expanded->we_wordv[2], &req->soft_limit_bytes) < 0 ||
	    parse_size_mb(expanded->we_wordv[3], &req->hard_limit_bytes) < 0) {
		fprintf(stderr, "invalid memory limit\n");
		wordfree(expanded);
		return -1;
	}

	if (req->soft_limit_bytes == 0 ||
	    req->hard_limit_bytes < req->soft_limit_bytes) {
		fprintf(stderr, "memory limits must satisfy 0 < soft <= hard\n");
		wordfree(expanded);
		return -1;
	}

	req->argv = &expanded->we_wordv[5];
	separator = strstr(line, "--");
	req->command_line = separator ? separator + 3 : expanded->we_wordv[5];

	return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
	const char *ptr = buf;

	while (len > 0) {
		ssize_t written = write(fd, ptr, len);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		ptr += written;
		len -= (size_t)written;
	}

	return 0;
}

static int child_exec_container(const struct child_exec_params *params)
{
	if (dup2(params->stdout_write_fd, STDOUT_FILENO) < 0)
		return -1;
	if (dup2(params->stderr_write_fd, STDERR_FILENO) < 0)
		return -1;

	close(params->stdout_write_fd);
	close(params->stderr_write_fd);

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	sethostname(params->req->name, strnlen(params->req->name, OSJ_NAME_LEN - 1));

	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0 && errno != EPERM)
		perror("mount private");
	if (mount("proc", "/proc", "proc", 0, NULL) < 0 &&
	    errno != EBUSY && errno != EPERM && errno != ENOENT)
		perror("mount /proc");

	execvp(params->req->argv[0], params->req->argv);
	perror("execvp");
	return -1;
}

static int launcher_wait_loop(pid_t init_pid)
{
	for (;;) {
		int status;
		pid_t waited;

		waited = waitpid(-1, &status, 0);
		if (waited < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}

		if (waited == init_pid) {
			if (WIFEXITED(status))
				return WEXITSTATUS(status);
			if (WIFSIGNALED(status))
				return 128 + WTERMSIG(status);
			return 1;
		}
	}
}

static int launcher_main(const struct start_request *req, int stdout_write_fd,
			 int stderr_write_fd, int control_write_fd)
{
	pid_t child;
	int exit_code;
	struct child_exec_params params;

	if (unshare(CLONE_NEWUTS | CLONE_NEWNS) < 0) {
		perror("unshare");
		return 1;
	}

	if (unshare(CLONE_NEWPID) < 0) {
		perror("unshare pid");
		return 1;
	}

	child = fork();
	if (child < 0) {
		perror("fork inner");
		return 1;
	}

	if (child == 0) {
		params.req = req;
		params.stdout_write_fd = stdout_write_fd;
		params.stderr_write_fd = stderr_write_fd;
		close(control_write_fd);
		return child_exec_container(&params);
	}

	if (write_full(control_write_fd, &child, sizeof(child)) < 0)
		perror("write init pid");

	close(control_write_fd);
	close(stdout_write_fd);
	close(stderr_write_fd);

	exit_code = launcher_wait_loop(child);
	return exit_code;
}

static int allocate_container_slot(struct runtime *rt, struct container **out)
{
	size_t i;

	pthread_mutex_lock(&rt->containers_lock);
	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		if (!rt->containers[i].used) {
			struct container *container = &rt->containers[i];

			memset(container, 0, sizeof(*container));
			container->used = true;
			container->state = CONTAINER_CREATED;
			container->stdout_fd = -1;
			container->stderr_fd = -1;
			container->log_fd = -1;
			container->id = ++rt->next_container_id;
			*out = container;
			pthread_mutex_unlock(&rt->containers_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&rt->containers_lock);

	return -1;
}

static void release_container_slot(struct container *container)
{
	if (!container)
		return;

	close_container_fds(container);
	memset(container, 0, sizeof(*container));
	container->stdout_fd = -1;
	container->stderr_fd = -1;
	container->log_fd = -1;
}

static int read_init_pid(int fd, pid_t *pid_out)
{
	size_t received = 0;
	char *buf = (char *)pid_out;

	while (received < sizeof(*pid_out)) {
		ssize_t n = read(fd, buf + received, sizeof(*pid_out) - received);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (n == 0)
			return -1;

		received += (size_t)n;
	}

	return 0;
}

static int start_container(struct runtime *rt, const struct start_request *req)
{
	struct container *container = NULL;
	int stdout_pipe[2] = { -1, -1 };
	int stderr_pipe[2] = { -1, -1 };
	int control_pipe[2] = { -1, -1 };
	pid_t launcher_pid = -1;
	pid_t init_pid = -1;

	if (allocate_container_slot(rt, &container) < 0) {
		fprintf(stderr, "container table full\n");
		return -1;
	}

	if (pipe2(stdout_pipe, O_CLOEXEC) < 0 ||
	    pipe2(stderr_pipe, O_CLOEXEC) < 0 ||
	    pipe2(control_pipe, O_CLOEXEC) < 0) {
		perror("pipe2");
		goto fail;
	}

	snprintf(container->name, sizeof(container->name), "%s", req->name);
	snprintf(container->command_line, sizeof(container->command_line), "%s",
		 req->command_line);
	snprintf(container->log_path, sizeof(container->log_path),
		 OSJ_LOG_DIR "/%u_%s.log", container->id, container->name);
	container->soft_limit_bytes = req->soft_limit_bytes;
	container->hard_limit_bytes = req->hard_limit_bytes;
	container->state = CONTAINER_STARTING;
	container->log_fd = open(container->log_path,
				 O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
	if (container->log_fd < 0) {
		perror("open log");
		goto fail;
	}

	launcher_pid = fork();
	if (launcher_pid < 0) {
		perror("fork");
		goto fail;
	}

	if (launcher_pid == 0) {
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		close(control_pipe[0]);
		_exit(launcher_main(req, stdout_pipe[1], stderr_pipe[1], control_pipe[1]));
	}

	close(stdout_pipe[1]);
	close(stderr_pipe[1]);
	close(control_pipe[1]);

	container->launcher_pid = launcher_pid;

	if (set_nonblocking(stdout_pipe[0]) < 0 || set_nonblocking(stderr_pipe[0]) < 0) {
		perror("set_nonblocking");
		goto fail;
	}

	if (read_init_pid(control_pipe[0], &init_pid) < 0) {
		fprintf(stderr, "failed to receive init pid for container %s\n",
			container->name);
		goto fail;
	}
	close(control_pipe[0]);
	control_pipe[0] = -1;

	container->init_pid = init_pid;
	container->stdout_fd = stdout_pipe[0];
	container->stderr_fd = stderr_pipe[0];
	container->state = CONTAINER_RUNNING;
	clock_gettime(CLOCK_REALTIME, &container->started_at);
	pthread_mutex_lock(&rt->containers_lock);
	int pending_status = 0;
	bool had_pending_exit =
		apply_pending_exit_locked(rt, container, &pending_status);
	pthread_mutex_unlock(&rt->containers_lock);
	if (had_pending_exit) {
		push_engine_log(rt, container->id,
				"container %s exited (launcher=%d init=%d status=0x%x)",
				container->name, container->launcher_pid,
				container->init_pid, pending_status);
	}

	if (register_io_source(rt, container->stdout_fd, container->id,
			       LOG_STREAM_STDOUT) < 0 ||
	    register_io_source(rt, container->stderr_fd, container->id,
			       LOG_STREAM_STDERR) < 0)
		goto fail;

	if (monitor_register_container(rt, container) == 0)
		container->registered_in_kernel = (rt->monitor_fd >= 0);

	push_engine_log(rt, container->id,
			"started name=%s launcher=%d init=%d soft=%lluMB hard=%lluMB cmd=%s",
			container->name, container->launcher_pid, container->init_pid,
			(unsigned long long)(container->soft_limit_bytes / (1024ULL * 1024ULL)),
			(unsigned long long)(container->hard_limit_bytes / (1024ULL * 1024ULL)),
			container->command_line);
	printf("started container %u (%s) init=%d launcher=%d\n",
	       container->id, container->name, container->init_pid,
	       container->launcher_pid);
	return 0;

fail:
	if (launcher_pid > 0) {
		kill(launcher_pid, SIGKILL);
		waitpid(launcher_pid, NULL, 0);
	}
	if (container->stdout_fd >= 0 && io_source_active(rt, container->stdout_fd))
		remove_io_source(rt, container->stdout_fd);
	if (container->stderr_fd >= 0 && io_source_active(rt, container->stderr_fd))
		remove_io_source(rt, container->stderr_fd);
	if (stdout_pipe[0] >= 0 && stdout_pipe[0] != container->stdout_fd)
		close(stdout_pipe[0]);
	if (stdout_pipe[1] >= 0)
		close(stdout_pipe[1]);
	if (stderr_pipe[0] >= 0 && stderr_pipe[0] != container->stderr_fd)
		close(stderr_pipe[0]);
	if (stderr_pipe[1] >= 0)
		close(stderr_pipe[1]);
	if (control_pipe[0] >= 0)
		close(control_pipe[0]);
	if (control_pipe[1] >= 0)
		close(control_pipe[1]);

	pthread_mutex_lock(&rt->containers_lock);
	release_container_slot(container);
	pthread_mutex_unlock(&rt->containers_lock);
	return -1;
}

static struct container *resolve_container_target_locked(struct runtime *rt,
							 const char *selector)
{
	char *end = NULL;
	unsigned long id;

	errno = 0;
	id = strtoul(selector, &end, 10);
	if (errno == 0 && end && *end == '\0')
		return find_container_by_id_locked(rt, (uint32_t)id);

	return find_container_by_name_locked(rt, selector);
}

static int stop_container(struct runtime *rt, const char *selector)
{
	struct container *container;
	int waited_ms = 0;

	pthread_mutex_lock(&rt->containers_lock);
	container = resolve_container_target_locked(rt, selector);
	if (!container) {
		pthread_mutex_unlock(&rt->containers_lock);
		fprintf(stderr, "unknown container: %s\n", selector);
		return -1;
	}

	if (container->state != CONTAINER_RUNNING &&
	    container->state != CONTAINER_STARTING &&
	    container->state != CONTAINER_STOPPING) {
		pthread_mutex_unlock(&rt->containers_lock);
		fprintf(stderr, "container %s is not running\n", selector);
		return -1;
	}

	container->state = CONTAINER_STOPPING;
	container->stop_requested = true;
	pthread_mutex_unlock(&rt->containers_lock);

	if (kill(container->init_pid, SIGTERM) < 0 && errno != ESRCH)
		perror("kill SIGTERM");

	for (waited_ms = 0; waited_ms < OSJ_STOP_WAIT_MS; waited_ms += 100) {
		pthread_mutex_lock(&rt->containers_lock);
		bool done = container->state == CONTAINER_EXITED ||
			container->state == CONTAINER_FAILED;
		pthread_mutex_unlock(&rt->containers_lock);
		if (done)
			break;
		usleep(100 * 1000);
	}

	pthread_mutex_lock(&rt->containers_lock);
	bool still_running = container->state == CONTAINER_RUNNING ||
		container->state == CONTAINER_STOPPING ||
		container->state == CONTAINER_STARTING;
	pthread_mutex_unlock(&rt->containers_lock);

	if (still_running) {
		if (kill(container->init_pid, SIGKILL) < 0 && errno != ESRCH)
			perror("kill SIGKILL");
		if (kill(container->launcher_pid, SIGKILL) < 0 && errno != ESRCH)
			perror("kill launcher");
	}

	push_engine_log(rt, container->id, "stop requested for %s", container->name);
	printf("stop issued for container %u (%s)\n", container->id, container->name);
	return 0;
}

static void print_container_row(const struct container *container)
{
	printf("%-4u %-16s %-10s %-8d %-8d %-10u %-10llu/%-10llu\n",
	       container->id, container->name, container_state_name(container->state),
	       container->launcher_pid, container->init_pid, container->member_count,
	       (unsigned long long)(container->last_rss_bytes / (1024ULL * 1024ULL)),
	       (unsigned long long)(container->hard_limit_bytes / (1024ULL * 1024ULL)));
}

static void list_containers(struct runtime *rt)
{
	size_t i;

	pthread_mutex_lock(&rt->containers_lock);
	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		if (rt->containers[i].used &&
		    (rt->containers[i].state == CONTAINER_RUNNING ||
		     rt->containers[i].state == CONTAINER_STOPPING))
			monitor_query_container(rt, &rt->containers[i]);
	}

	printf("ID   NAME             STATE      LAUNCHER INIT     MEMBERS    RSS_MB/HARD\n");
	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		if (rt->containers[i].used)
			print_container_row(&rt->containers[i]);
	}
	pthread_mutex_unlock(&rt->containers_lock);
}

static void stop_all_containers(struct runtime *rt)
{
	size_t i;
	char selector[32];

	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		pthread_mutex_lock(&rt->containers_lock);
		if (!rt->containers[i].used ||
		    (rt->containers[i].state != CONTAINER_RUNNING &&
		     rt->containers[i].state != CONTAINER_STARTING &&
		     rt->containers[i].state != CONTAINER_STOPPING)) {
			pthread_mutex_unlock(&rt->containers_lock);
			continue;
		}
		snprintf(selector, sizeof(selector), "%u", rt->containers[i].id);
		pthread_mutex_unlock(&rt->containers_lock);
		stop_container(rt, selector);
	}
}

static void runtime_cleanup(struct runtime *rt)
{
	size_t i;

	stop_all_containers(rt);
	reap_children(rt);
	atomic_store(&rt->shutdown_requested, true);
	kill(getpid(), SIGTERM);
	log_queue_close(&rt->log_queue);

	pthread_join(rt->signal_thread, NULL);
	pthread_join(rt->io_thread, NULL);
	pthread_join(rt->logger_thread, NULL);

	for (i = 0; i < OSJ_MAX_CONTAINERS; i++) {
		if (!rt->containers[i].used)
			continue;
		if (rt->containers[i].registered_in_kernel)
			monitor_unregister_container(rt, rt->containers[i].id);
		close_container_fds(&rt->containers[i]);
	}

	if (rt->monitor_fd >= 0)
		close(rt->monitor_fd);
	if (rt->epoll_fd >= 0)
		close(rt->epoll_fd);
}

static int runtime_init(struct runtime *rt)
{
	memset(rt, 0, sizeof(*rt));
	rt->epoll_fd = -1;
	rt->monitor_fd = -1;
	pthread_mutex_init(&rt->containers_lock, NULL);
	log_queue_init(&rt->log_queue);

	if (mkdir_if_missing(OSJ_LOG_DIR) < 0)
		return -1;

	sigemptyset(&rt->signal_set);
	sigaddset(&rt->signal_set, SIGCHLD);
	sigaddset(&rt->signal_set, SIGINT);
	sigaddset(&rt->signal_set, SIGTERM);
	if (pthread_sigmask(SIG_BLOCK, &rt->signal_set, NULL) != 0) {
		perror("pthread_sigmask");
		return -1;
	}

	rt->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (rt->epoll_fd < 0) {
		perror("epoll_create1");
		return -1;
	}

	rt->monitor_fd = monitor_open();

	if (pthread_create(&rt->logger_thread, NULL, logger_thread_main, rt) != 0) {
		perror("pthread_create logger");
		return -1;
	}

	if (pthread_create(&rt->io_thread, NULL, io_thread_main, rt) != 0) {
		perror("pthread_create io");
		return -1;
	}

	if (pthread_create(&rt->signal_thread, NULL, signal_thread_main, rt) != 0) {
		perror("pthread_create signal");
		return -1;
	}

	return 0;
}

static void print_help(void)
{
	printf("commands:\n");
	printf("  start <name> <soft-limit-mb> <hard-limit-mb> -- <command> [args...]\n");
	printf("  stop <container-id|name>\n");
	printf("  list\n");
	printf("  help\n");
	printf("  quit\n");
}

static int handle_command(struct runtime *rt, char *line)
{
	wordexp_t expanded;
	struct start_request start_req;

	if (strncmp(line, "help", 4) == 0) {
		print_help();
		return 0;
	}

	if (strncmp(line, "list", 4) == 0) {
		list_containers(rt);
		return 0;
	}

	if (strncmp(line, "quit", 4) == 0 || strncmp(line, "exit", 4) == 0) {
		request_shutdown(rt);
		return 0;
	}

	if (strncmp(line, "stop ", 5) == 0) {
		memset(&expanded, 0, sizeof(expanded));
		if (wordexp(line, &expanded, WRDE_NOCMD) != 0) {
			fprintf(stderr, "failed to parse stop command\n");
			return -1;
		}
		if (expanded.we_wordc != 2) {
			fprintf(stderr, "usage: stop <container-id|name>\n");
			wordfree(&expanded);
			return -1;
		}
		stop_container(rt, expanded.we_wordv[1]);
		wordfree(&expanded);
		return 0;
	}

	if (strncmp(line, "start ", 6) == 0) {
		if (parse_start_request(line, &start_req, &expanded) < 0)
			return -1;
		start_container(rt, &start_req);
		wordfree(&expanded);
		return 0;
	}

	fprintf(stderr, "unknown command; try 'help'\n");
	return -1;
}

static int supervisor_loop(struct runtime *rt)
{
	char *line = NULL;
	size_t cap = 0;

	print_help();

	while (!atomic_load(&rt->shutdown_requested)) {
		struct pollfd pfd;
		int rc;

		pfd.fd = STDIN_FILENO;
		pfd.events = POLLIN;
		pfd.revents = 0;

		rc = poll(&pfd, 1, 500);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		if (rc == 0)
			continue;

		if (getline(&line, &cap, stdin) < 0) {
			request_shutdown(rt);
			break;
		}

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '\0')
			continue;

		handle_command(rt, line);
	}

	free(line);
	return 0;
}

int main(void)
{
	struct runtime runtime;

	if (runtime_init(&runtime) < 0)
		return 1;

	supervisor_loop(&runtime);
	runtime_cleanup(&runtime);
	return 0;
}
