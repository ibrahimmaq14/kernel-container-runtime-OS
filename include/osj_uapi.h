#ifndef OSJ_UAPI_H
#define OSJ_UAPI_H

/*
 * OS-Jackfruit
 * Authors:
 *   Mohammed Ibrahim Maqsood Khan (PES1UG24C578)
 *   Nitesh Harsur (PES1UG24CS584)
 */

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#if defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
#endif
#endif

#define OSJ_ABI_VERSION 1U
#define OSJ_NAME_LEN 64U

#define OSJ_FLAG_REGISTERED    (1U << 0)
#define OSJ_FLAG_SOFT_EXCEEDED (1U << 1)
#define OSJ_FLAG_HARD_EXCEEDED (1U << 2)
#define OSJ_FLAG_INIT_ALIVE    (1U << 3)

struct osj_container_register_req {
	__u32 abi_version;
	__u32 container_id;
	__s32 init_pid;
	__u32 reserved;
	__u64 soft_limit_bytes;
	__u64 hard_limit_bytes;
	char name[OSJ_NAME_LEN];
};

struct osj_container_unregister_req {
	__u32 abi_version;
	__u32 container_id;
};

struct osj_container_query_req {
	__u32 abi_version;
	__u32 container_id;
	__u64 current_rss_bytes;
	__u64 peak_rss_bytes;
	__u32 member_count;
	__u32 flags;
};

#define OSJ_IOC_MAGIC 'o'

#define OSJ_IOC_REGISTER_CONTAINER   \
	_IOW(OSJ_IOC_MAGIC, 1, struct osj_container_register_req)
#define OSJ_IOC_UNREGISTER_CONTAINER \
	_IOW(OSJ_IOC_MAGIC, 2, struct osj_container_unregister_req)
#define OSJ_IOC_QUERY_CONTAINER      \
	_IOWR(OSJ_IOC_MAGIC, 3, struct osj_container_query_req)

#endif /* OSJ_UAPI_H */
