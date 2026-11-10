// SPDX-License-Identifier: GPL-2.0
/*
 * mychar: a small character device exposing a resizable byte buffer.
 * read()/write() use a per-open file position; ioctls allow resizing and
 * random-access transfer with user supplied offset/length.
 *
 * Locking: mydev.lock protects buf, size and resize_count. It is never held
 * across a user-memory access, so a slow page fault (userfaultfd, FUSE) in one
 * task cannot stall every other user of this world-accessible device. Data
 * moves through a per-call bounce buffer instead. The byte counters are
 * atomic so they can be updated after the user copy has succeeded.
 */
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include "mychar.h"

#define MYCHAR_DEFAULT_SIZE 4096

struct mychar_dev {
	struct mutex lock;
	u8 *buf;
	size_t size;
	u32 resize_count;
	atomic64_t bytes_written;
	atomic64_t bytes_read;
};

static struct mychar_dev mydev;

/* Takes u64 so an oversized request cannot be truncated before the check. */
static int mychar_resize_locked(u64 new_size)
{
	u8 *nb = NULL;

	lockdep_assert_held(&mydev.lock);

	if (new_size > MYCHAR_MAX_SIZE)
		return -EINVAL;
	if (new_size == mydev.size)
		return 0;

	if (new_size) {
		nb = kvzalloc(new_size, GFP_KERNEL);
		if (!nb)
			return -ENOMEM;
		if (mydev.buf)
			memcpy(nb, mydev.buf, min_t(u64, mydev.size, new_size));
	}

	kvfree(mydev.buf);
	mydev.buf = nb;
	WRITE_ONCE(mydev.size, new_size);
	mydev.resize_count++;
	return 0;
}

/*
 * Move up to @len bytes between user memory and the device at @off.
 *
 * @strict selects the ioctl contract: the whole range must fit or the call
 * fails with -EINVAL. Otherwise read()/write() semantics apply: the transfer
 * is clamped to the end of the buffer, and starting at or past the end gives
 * 0 for a read and -ENOSPC for a write. A negative file offset arrives here
 * as a huge u64 and takes the past-the-end path.
 *
 * Returns the number of bytes transferred or a negative errno.
 */
static ssize_t mychar_transfer(void __user *uptr, u64 off, u64 len,
			       bool is_write, bool strict)
{
	u8 *bounce = NULL;
	ssize_t ret;
	size_t n;

	if (strict) {
		if (len > MYCHAR_MAX_SIZE)
			return -EINVAL;
		n = len;
	} else {
		/*
		 * Unlocked size hint: limits the allocation and avoids
		 * touching user memory the transfer will not use. The range
		 * is validated again under the lock.
		 */
		u64 size = READ_ONCE(mydev.size);

		n = off < size ? min_t(u64, len, size - off) : 0;
	}

	if (n) {
		bounce = kvmalloc(n, GFP_KERNEL);
		if (!bounce)
			return -ENOMEM;
		if (is_write && copy_from_user(bounce, uptr, n)) {
			ret = -EFAULT;
			goto out_free;
		}
	}

	ret = mutex_lock_interruptible(&mydev.lock);
	if (ret)
		goto out_free;

	if (strict) {
		if (off > mydev.size || n > mydev.size - off) {
			ret = -EINVAL;
			goto out_unlock;
		}
	} else if (off >= mydev.size) {
		ret = is_write ? -ENOSPC : 0;
		goto out_unlock;
	} else {
		n = min_t(u64, n, mydev.size - off);
	}

	if (n) {
		if (is_write)
			memcpy(mydev.buf + off, bounce, n);
		else
			memcpy(bounce, mydev.buf + off, n);
	}
	ret = n;

out_unlock:
	mutex_unlock(&mydev.lock);

	if (ret > 0 && !is_write && copy_to_user(uptr, bounce, ret))
		ret = -EFAULT;
	if (ret > 0)
		atomic64_add(ret, is_write ? &mydev.bytes_written :
					     &mydev.bytes_read);
out_free:
	kvfree(bounce);
	return ret;
}

static ssize_t mychar_read(struct file *filp, char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	ssize_t ret = mychar_transfer(ubuf, *ppos, count, false, false);

	if (ret > 0)
		*ppos += ret;
	return ret;
}

static ssize_t mychar_write(struct file *filp, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	ssize_t ret = mychar_transfer((void __user *)ubuf, *ppos, count,
				      true, false);

	if (ret > 0)
		*ppos += ret;
	return ret;
}

static long mychar_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	struct mychar_xfer x;
	struct mychar_stat st;
	u64 new_size;
	ssize_t n;
	long ret;

	switch (cmd) {
	case MYCHAR_IOC_RESIZE:
		if (copy_from_user(&new_size, uarg, sizeof(new_size)))
			return -EFAULT;
		ret = mutex_lock_interruptible(&mydev.lock);
		if (ret)
			return ret;
		ret = mychar_resize_locked(new_size);
		mutex_unlock(&mydev.lock);
		return ret;
	case MYCHAR_IOC_WRITE_AT:
	case MYCHAR_IOC_READ_AT:
		if (copy_from_user(&x, uarg, sizeof(x)))
			return -EFAULT;
		n = mychar_transfer(u64_to_user_ptr(x.user_ptr), x.offset,
				    x.len, cmd == MYCHAR_IOC_WRITE_AT, true);
		return n < 0 ? n : 0;
	case MYCHAR_IOC_RESET:
		ret = mutex_lock_interruptible(&mydev.lock);
		if (ret)
			return ret;
		if (mydev.buf)
			memset(mydev.buf, 0, mydev.size);
		mutex_unlock(&mydev.lock);
		atomic64_set(&mydev.bytes_read, 0);
		atomic64_set(&mydev.bytes_written, 0);
		return 0;
	case MYCHAR_IOC_STAT:
		memset(&st, 0, sizeof(st));
		ret = mutex_lock_interruptible(&mydev.lock);
		if (ret)
			return ret;
		st.size = mydev.size;
		st.resize_count = mydev.resize_count;
		mutex_unlock(&mydev.lock);
		st.bytes_written = atomic64_read(&mydev.bytes_written);
		st.bytes_read = atomic64_read(&mydev.bytes_read);
		return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations mychar_fops = {
	.owner		= THIS_MODULE,
	.read		= mychar_read,
	.write		= mychar_write,
	.unlocked_ioctl	= mychar_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= default_llseek,
};

static struct miscdevice mychar_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "mychar",
	.fops	= &mychar_fops,
	.mode	= 0666,
};

static int __init mychar_init(void)
{
	int ret;

	mutex_init(&mydev.lock);

	mutex_lock(&mydev.lock);
	ret = mychar_resize_locked(MYCHAR_DEFAULT_SIZE);
	mutex_unlock(&mydev.lock);
	if (ret)
		goto err_mutex;

	ret = misc_register(&mychar_misc);
	if (ret)
		goto err_buf;
	return 0;

err_buf:
	kvfree(mydev.buf);
	mydev.buf = NULL;
err_mutex:
	mutex_destroy(&mydev.lock);
	return ret;
}

static void __exit mychar_exit(void)
{
	misc_deregister(&mychar_misc);
	kvfree(mydev.buf);
	mutex_destroy(&mydev.lock);
}

module_init(mychar_init);
module_exit(mychar_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Resizable buffer char device for fuzzing practice");
