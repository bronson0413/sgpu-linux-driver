/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MYCHAR_H
#define MYCHAR_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MYCHAR_MAX_SIZE (1u << 20)

struct mychar_xfer {
	__u64 offset;	/* byte offset inside the device buffer */
	__u64 len;	/* number of bytes */
	__u64 user_ptr;	/* user-space buffer */
};

struct mychar_stat {
	__u64 size;
	__u64 bytes_written;
	__u64 bytes_read;
	__u32 resize_count;
	__u32 pad;
};

#define MYCHAR_IOC_MAGIC 'M'
#define MYCHAR_IOC_RESIZE	_IOW(MYCHAR_IOC_MAGIC, 1, __u64)
#define MYCHAR_IOC_WRITE_AT	_IOW(MYCHAR_IOC_MAGIC, 2, struct mychar_xfer)
#define MYCHAR_IOC_READ_AT	_IOW(MYCHAR_IOC_MAGIC, 3, struct mychar_xfer)
#define MYCHAR_IOC_RESET	_IO(MYCHAR_IOC_MAGIC, 4)
#define MYCHAR_IOC_STAT		_IOR(MYCHAR_IOC_MAGIC, 5, struct mychar_stat)

#endif
