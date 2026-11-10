/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* User-space interface of /dev/sgpu. Every open file is one GPU context. */
#ifndef SGPU_UAPI_H
#define SGPU_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SGPU_MAX_BO_SIZE	(1u << 20)
#define SGPU_MAX_CMDS		64

#define SGPU_OP_NOP		0u
#define SGPU_OP_FILL		1u	/* fill dst[len] with the byte in src */
#define SGPU_OP_COPY		2u	/* copy src[len] to dst[len] */
/* Only accepted when the module is loaded with test_ops=1. */
#define SGPU_OP_TEST_HANG	0x100u
#define SGPU_OP_TEST_FAULT	0x101u

struct sgpu_info {
	__u32 version;
	__u32 num_contexts;
	__u32 max_bo_size;
	__u32 context_id;
	__u32 banned;
	__u32 resets;
	__u32 test_ops;
	__u32 pad;
};

struct sgpu_bo_create {
	__u64 size;	/* in */
	__u64 gpu_va;	/* out */
	__u32 handle;	/* out */
	__u32 pad;
};

struct sgpu_bo_destroy {
	__u32 handle;
	__u32 pad;
};

struct sgpu_bo_xfer {
	__u64 offset;
	__u64 len;
	__u64 user_ptr;
	__u32 handle;
	__u32 pad;
};

struct sgpu_cmd {
	__u64 src;
	__u64 dst;
	__u32 opcode;
	__u32 len;
};

struct sgpu_submit {
	__u64 cmds_ptr;	/* array of struct sgpu_cmd */
	__u64 fence;	/* out */
	__u32 count;
	__u32 pad;
};

struct sgpu_wait {
	__u64 fence;
	__u32 timeout_ms;
	__s32 status;	/* out: 0 or the negative errno of the failed job */
};

#define SGPU_IOC_MAGIC 'G'
#define SGPU_IOC_INFO		_IOR(SGPU_IOC_MAGIC, 1, struct sgpu_info)
#define SGPU_IOC_BO_CREATE	_IOWR(SGPU_IOC_MAGIC, 2, struct sgpu_bo_create)
#define SGPU_IOC_BO_DESTROY	_IOW(SGPU_IOC_MAGIC, 3, struct sgpu_bo_destroy)
#define SGPU_IOC_BO_WRITE	_IOW(SGPU_IOC_MAGIC, 4, struct sgpu_bo_xfer)
#define SGPU_IOC_BO_READ	_IOW(SGPU_IOC_MAGIC, 5, struct sgpu_bo_xfer)
#define SGPU_IOC_SUBMIT		_IOWR(SGPU_IOC_MAGIC, 6, struct sgpu_submit)
#define SGPU_IOC_WAIT		_IOWR(SGPU_IOC_MAGIC, 7, struct sgpu_wait)

#endif
