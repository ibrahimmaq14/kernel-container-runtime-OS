/*
 * OS-Jackfruit kernel monitor
 * Authors:
 *   Mohammed Ibrahim Maqsood Khan (PES1UG24C578)
 *   Nitesh Harsur (PES1UG24CS584)
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "../include/osj_uapi.h"

#define OSJ_SCAN_INTERVAL_MS 1000

struct osj_tracked_container {
	struct list_head link;
	u32 container_id;
	char name[OSJ_NAME_LEN];
	struct pid *init_pid_ref;
	struct pid_namespace *pid_ns;
	u64 soft_limit_bytes;
	u64 hard_limit_bytes;
	u64 last_rss_bytes;
	u64 peak_rss_bytes;
	u32 member_count;
	bool soft_warned;
	bool hard_killed;
};

struct osj_monitor_device {
	struct miscdevice miscdev;
	struct delayed_work scan_work;
	struct list_head containers;
	struct mutex lock;
};

static struct osj_monitor_device osj_monitor;

static struct osj_tracked_container *
osj_find_container_locked(u32 container_id)
{
	struct osj_tracked_container *entry;

	list_for_each_entry(entry, &osj_monitor.containers, link) {
		if (entry->container_id == container_id)
			return entry;
	}

	return NULL;
}

static u64 osj_sample_namespace_rss(struct osj_tracked_container *entry, u32 *members)
{
	struct task_struct *task;
	u64 total = 0;
	u32 count = 0;

	rcu_read_lock();
	for_each_process(task) {
		struct mm_struct *mm;

		if (task_active_pid_ns(task) != entry->pid_ns)
			continue;

		mm = get_task_mm(task);
		if (!mm)
			continue;

		total += (u64)get_mm_rss(mm) << PAGE_SHIFT;
		count++;
		mmput(mm);
	}
	rcu_read_unlock();

	*members = count;
	return total;
}

static void osj_kill_namespace(struct osj_tracked_container *entry)
{
	struct task_struct *task;

	rcu_read_lock();
	for_each_process(task) {
		if (task_active_pid_ns(task) != entry->pid_ns)
			continue;

		send_sig(SIGKILL, task, 1);
	}
	rcu_read_unlock();
}

static bool osj_init_task_alive(struct osj_tracked_container *entry)
{
	struct task_struct *task;
	bool alive = false;

	rcu_read_lock();
	task = pid_task(entry->init_pid_ref, PIDTYPE_PID);
	if (task)
		alive = pid_alive(task);
	rcu_read_unlock();

	return alive;
}

static void osj_enforce_limits(struct osj_tracked_container *entry)
{
	if (entry->hard_limit_bytes &&
	    entry->last_rss_bytes >= entry->hard_limit_bytes &&
	    !entry->hard_killed) {
		pr_warn("osj: container %u (%s) exceeded hard limit: rss=%llu hard=%llu\n",
			entry->container_id, entry->name,
			(unsigned long long)entry->last_rss_bytes,
			(unsigned long long)entry->hard_limit_bytes);
		osj_kill_namespace(entry);
		entry->hard_killed = true;
		return;
	}

	if (entry->soft_limit_bytes &&
	    entry->last_rss_bytes >= entry->soft_limit_bytes) {
		if (!entry->soft_warned) {
			pr_warn("osj: container %u (%s) exceeded soft limit: rss=%llu soft=%llu\n",
				entry->container_id, entry->name,
				(unsigned long long)entry->last_rss_bytes,
				(unsigned long long)entry->soft_limit_bytes);
			entry->soft_warned = true;
		}
	} else {
		entry->soft_warned = false;
	}
}

static void osj_scan_workfn(struct work_struct *work)
{
	struct osj_tracked_container *entry;
	struct osj_tracked_container *tmp;

	mutex_lock(&osj_monitor.lock);
	list_for_each_entry_safe(entry, tmp, &osj_monitor.containers, link) {
		entry->last_rss_bytes =
			osj_sample_namespace_rss(entry, &entry->member_count);
		if (entry->last_rss_bytes > entry->peak_rss_bytes)
			entry->peak_rss_bytes = entry->last_rss_bytes;

		osj_enforce_limits(entry);

		if (!osj_init_task_alive(entry) && entry->member_count == 0) {
			list_del(&entry->link);
			put_pid(entry->init_pid_ref);
			put_pid_ns(entry->pid_ns);
			kfree(entry);
		}
	}
	mutex_unlock(&osj_monitor.lock);

	schedule_delayed_work(&osj_monitor.scan_work,
			      msecs_to_jiffies(OSJ_SCAN_INTERVAL_MS));
}

static long osj_register_container(unsigned long arg)
{
	struct osj_container_register_req req;
	struct osj_tracked_container *entry;
	struct task_struct *task;
	struct pid *pid_ref;
	struct pid_namespace *pid_ns;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.abi_version != OSJ_ABI_VERSION)
		return -EINVAL;
	if (req.container_id == 0 || req.init_pid <= 0)
		return -EINVAL;
	if (req.hard_limit_bytes < req.soft_limit_bytes)
		return -EINVAL;

	pid_ref = find_get_pid(req.init_pid);
	if (!pid_ref)
		return -ESRCH;

	task = get_pid_task(pid_ref, PIDTYPE_PID);
	if (!task) {
		put_pid(pid_ref);
		return -ESRCH;
	}

	pid_ns = task_active_pid_ns(task);
	if (!pid_ns) {
		put_task_struct(task);
		put_pid(pid_ref);
		return -EINVAL;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		put_task_struct(task);
		put_pid(pid_ref);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&entry->link);
	entry->container_id = req.container_id;
	strscpy(entry->name, req.name, sizeof(entry->name));
	entry->init_pid_ref = pid_ref;
	entry->pid_ns = get_pid_ns(pid_ns);
	entry->soft_limit_bytes = req.soft_limit_bytes;
	entry->hard_limit_bytes = req.hard_limit_bytes;

	mutex_lock(&osj_monitor.lock);
	if (osj_find_container_locked(req.container_id)) {
		mutex_unlock(&osj_monitor.lock);
		put_task_struct(task);
		put_pid(pid_ref);
		put_pid_ns(entry->pid_ns);
		kfree(entry);
		return -EEXIST;
	}
	list_add_tail(&entry->link, &osj_monitor.containers);
	mutex_unlock(&osj_monitor.lock);

	put_task_struct(task);
	return 0;
}

static long osj_unregister_container(unsigned long arg)
{
	struct osj_container_unregister_req req;
	struct osj_tracked_container *entry;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.abi_version != OSJ_ABI_VERSION || req.container_id == 0)
		return -EINVAL;

	mutex_lock(&osj_monitor.lock);
	entry = osj_find_container_locked(req.container_id);
	if (!entry) {
		mutex_unlock(&osj_monitor.lock);
		return -ENOENT;
	}

	list_del(&entry->link);
	mutex_unlock(&osj_monitor.lock);

	put_pid(entry->init_pid_ref);
	put_pid_ns(entry->pid_ns);
	kfree(entry);
	return 0;
}

static long osj_query_container(unsigned long arg)
{
	struct osj_container_query_req req;
	struct osj_tracked_container *entry;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.abi_version != OSJ_ABI_VERSION || req.container_id == 0)
		return -EINVAL;

	mutex_lock(&osj_monitor.lock);
	entry = osj_find_container_locked(req.container_id);
	if (!entry) {
		mutex_unlock(&osj_monitor.lock);
		return -ENOENT;
	}

	req.current_rss_bytes = entry->last_rss_bytes;
	req.peak_rss_bytes = entry->peak_rss_bytes;
	req.member_count = entry->member_count;
	req.flags = OSJ_FLAG_REGISTERED;
	if (entry->soft_warned)
		req.flags |= OSJ_FLAG_SOFT_EXCEEDED;
	if (entry->hard_killed)
		req.flags |= OSJ_FLAG_HARD_EXCEEDED;
	if (osj_init_task_alive(entry))
		req.flags |= OSJ_FLAG_INIT_ALIVE;
	mutex_unlock(&osj_monitor.lock);

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long osj_monitor_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	switch (cmd) {
	case OSJ_IOC_REGISTER_CONTAINER:
		return osj_register_container(arg);
	case OSJ_IOC_UNREGISTER_CONTAINER:
		return osj_unregister_container(arg);
	case OSJ_IOC_QUERY_CONTAINER:
		return osj_query_container(arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations osj_monitor_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = osj_monitor_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = osj_monitor_ioctl,
#endif
};

static int __init osj_monitor_init(void)
{
	int ret;

	INIT_LIST_HEAD(&osj_monitor.containers);
	mutex_init(&osj_monitor.lock);
	INIT_DELAYED_WORK(&osj_monitor.scan_work, osj_scan_workfn);
	osj_monitor.miscdev.minor = MISC_DYNAMIC_MINOR;
	osj_monitor.miscdev.name = "osj_monitor";
	osj_monitor.miscdev.fops = &osj_monitor_fops;
	osj_monitor.miscdev.mode = 0666;

	ret = misc_register(&osj_monitor.miscdev);
	if (ret)
		return ret;

	schedule_delayed_work(&osj_monitor.scan_work,
			      msecs_to_jiffies(OSJ_SCAN_INTERVAL_MS));
	pr_info("osj monitor loaded\n");
	return 0;
}

static void __exit osj_monitor_exit(void)
{
	struct osj_tracked_container *entry;
	struct osj_tracked_container *tmp;

	cancel_delayed_work_sync(&osj_monitor.scan_work);
	misc_deregister(&osj_monitor.miscdev);

	mutex_lock(&osj_monitor.lock);
	list_for_each_entry_safe(entry, tmp, &osj_monitor.containers, link) {
		list_del(&entry->link);
		put_pid(entry->init_pid_ref);
		put_pid_ns(entry->pid_ns);
		kfree(entry);
	}
	mutex_unlock(&osj_monitor.lock);

	pr_info("osj monitor unloaded\n");
}

module_init(osj_monitor_init);
module_exit(osj_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mohammed Ibrahim Maqsood Khan (PES1UG24C578), Nitesh Harsur (PES1UG24CS584)");
MODULE_DESCRIPTION("OS-Jackfruit container memory monitor");
