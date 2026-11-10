// SPDX-License-Identifier: GPL-2.0
/*
 * sgpu: Linux driver for the SGPU command processor.
 *
 * Every open file is an isolated GPU context with its own page table and
 * buffer objects. User commands are untrusted: they are validated against the
 * context's own buffers, and the context index is always filled in by the
 * kernel. See docs/threat-model.md for the reasoning behind these rules.
 *
 * Object lifetime:
 *   sgpu_device <- sgpu_ctx <- sgpu_bo <- sgpu_job (each holds the one before)
 * A buffer stays mapped until the last job that references it has finished,
 * even if user space destroyed its handle or closed the file meanwhile.
 *
 * Locking, outermost first:
 *   remove_sem	read side around device access, write side in sgpu_remove()
 *   ctx->lock	buffer handles, page table and VA allocator of one context
 *   dev->lock	ring, job list, context slots and ban state
 * remove_sem is not held across user-memory access or while waiting for a
 * fence, so a stalled task cannot block device removal.
 */
#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "sgpu_hw.h"
#include "sgpu_uapi.h"

#define SGPU_RING_ENTRIES	256
#define SGPU_MAX_FAILURES	64
#define SGPU_MAX_HANDLES	1024
#define SGPU_MAX_WAIT_MS	60000
#define SGPU_RING_WAIT		(5 * HZ)

static bool test_ops;
module_param(test_ops, bool, 0444);
MODULE_PARM_DESC(test_ops, "Accept the hang and fault test commands");

static unsigned int timeout_ms = 2000;
module_param(timeout_ms, uint, 0444);
MODULE_PARM_DESC(timeout_ms, "Reset the device after this long without progress");

struct sgpu_hw_cmd {
	__le32 opcode;
	__le32 ctx;
	__le64 src;
	__le64 dst;
	__le32 len;
	__le32 fence;
};

struct sgpu_failure {
	u64 fence;
	int err;
};

struct sgpu_device {
	struct kref ref;
	struct pci_dev *pdev;
	void __iomem *regs;
	struct miscdevice misc;
	struct dentry *debugfs;
	struct delayed_work watchdog;
	wait_queue_head_t wq;

	struct rw_semaphore remove_sem;
	bool gone;		/* written under remove_sem for write */
	bool dying;		/* set before remove_sem is taken for write */

	struct mutex lock;
	struct sgpu_hw_cmd *ring;
	dma_addr_t ring_dma;
	__le64 *ctx_table;
	dma_addr_t ctx_table_dma;
	struct sgpu_ctx *ctx[SGPU_NUM_CTX];
	struct list_head jobs;
	u32 ring_tail;
	u32 ring_used;
	u64 next_seq;
	unsigned long last_progress;
	u32 resets;

	atomic64_t completed;
};

struct sgpu_ctx {
	struct kref ref;
	struct sgpu_device *dev;
	u32 slot;

	struct mutex lock;
	__le64 *pt;
	dma_addr_t pt_dma;
	struct xarray bos;
	DECLARE_BITMAP(va_used, SGPU_VA_PAGES);
	struct sgpu_bo *page_bo[SGPU_VA_PAGES];

	/* protected by dev->lock */
	bool banned;
	u64 last_fence;
	struct sgpu_failure failures[SGPU_MAX_FAILURES];
	u32 failure_next;
};

struct sgpu_bo {
	struct kref ref;
	struct sgpu_ctx *ctx;
	void *vaddr;
	dma_addr_t dma;
	size_t size;
	u32 first_page;
	u32 npages;
};

struct sgpu_job {
	struct list_head node;
	struct sgpu_ctx *ctx;
	u64 seq;
	u32 ncmds;
	u32 nbos;
	struct sgpu_hw_cmd *cmds;
	struct sgpu_bo *bos[];
};

static inline void sgpu_wr(struct sgpu_device *dev, u32 reg, u32 val)
{
	writel(val, dev->regs + reg);
}

static inline u32 sgpu_rd(struct sgpu_device *dev, u32 reg)
{
	return readl(dev->regs + reg);
}

static int sgpu_enter(struct sgpu_device *dev)
{
	int ret = down_read_killable(&dev->remove_sem);

	if (ret)
		return ret;
	if (dev->gone) {
		up_read(&dev->remove_sem);
		return -ENODEV;
	}
	return 0;
}

static void sgpu_leave(struct sgpu_device *dev)
{
	up_read(&dev->remove_sem);
}

/* Object release */

static void sgpu_dev_release(struct kref *ref)
{
	kfree(container_of(ref, struct sgpu_device, ref));
}

static void sgpu_ctx_release(struct kref *ref)
{
	struct sgpu_ctx *ctx = container_of(ref, struct sgpu_ctx, ref);
	struct sgpu_device *dev = ctx->dev;

	mutex_lock(&dev->lock);
	dev->ctx[ctx->slot] = NULL;
	if (!dev->gone)
		dev->ctx_table[ctx->slot] = 0;
	mutex_unlock(&dev->lock);

	/* After removal the page table has already been revoked. */
	if (ctx->pt)
		dma_free_coherent(&dev->pdev->dev,
				  SGPU_VA_PAGES * sizeof(*ctx->pt), ctx->pt,
				  ctx->pt_dma);
	xa_destroy(&ctx->bos);
	mutex_destroy(&ctx->lock);
	kfree(ctx);
	kref_put(&dev->ref, sgpu_dev_release);
}

static void sgpu_ctx_put(struct sgpu_ctx *ctx)
{
	kref_put(&ctx->ref, sgpu_ctx_release);
}

static void sgpu_bo_release(struct kref *ref)
{
	struct sgpu_bo *bo = container_of(ref, struct sgpu_bo, ref);
	struct sgpu_ctx *ctx = bo->ctx;
	u32 i;

	mutex_lock(&ctx->lock);
	for (i = 0; i < bo->npages; i++)
		if (ctx->pt)
			ctx->pt[bo->first_page + i] = 0;
	bitmap_clear(ctx->va_used, bo->first_page, bo->npages + 1);
	mutex_unlock(&ctx->lock);

	if (bo->vaddr)
		dma_free_coherent(&ctx->dev->pdev->dev, bo->size, bo->vaddr,
				  bo->dma);
	kfree(bo);
	sgpu_ctx_put(ctx);
}

static void sgpu_bo_put(struct sgpu_bo *bo)
{
	kref_put(&bo->ref, sgpu_bo_release);
}

static void sgpu_job_free(struct sgpu_job *job)
{
	u32 i;

	for (i = 0; i < job->nbos; i++)
		sgpu_bo_put(job->bos[i]);
	sgpu_ctx_put(job->ctx);
	kfree(job->cmds);
	kfree(job);
}

static void sgpu_jobs_free(struct list_head *done)
{
	struct sgpu_job *job, *tmp;

	list_for_each_entry_safe(job, tmp, done, node) {
		list_del(&job->node);
		sgpu_job_free(job);
	}
}

/*
 * Remove a handle and its address lookup entries together. page_bo[] borrows
 * the reference owned by the handle, so an entry must never outlive it. The
 * last reference can be dropped outside any ioctl, by job completion in the
 * interrupt thread or watchdog, and a submit must not find a buffer in that
 * state. The page table stays valid until release, for jobs still in flight.
 */
static struct sgpu_bo *sgpu_bo_unpublish_locked(struct sgpu_ctx *ctx,
						unsigned long handle)
{
	struct sgpu_bo *bo;
	u32 i;

	lockdep_assert_held(&ctx->lock);
	bo = xa_erase(&ctx->bos, handle);
	for (i = 0; bo && i < bo->npages; i++)
		ctx->page_bo[bo->first_page + i] = NULL;
	return bo;
}

/* Hardware control and job completion */

static void sgpu_hw_start(struct sgpu_device *dev)
{
	sgpu_wr(dev, SGPU_REG_CTRL, SGPU_CTRL_RESET);
	sgpu_wr(dev, SGPU_REG_RING_BASE_LO, lower_32_bits(dev->ring_dma));
	sgpu_wr(dev, SGPU_REG_RING_BASE_HI, upper_32_bits(dev->ring_dma));
	sgpu_wr(dev, SGPU_REG_RING_SIZE, SGPU_RING_ENTRIES);
	sgpu_wr(dev, SGPU_REG_CTX_TABLE_LO, lower_32_bits(dev->ctx_table_dma));
	sgpu_wr(dev, SGPU_REG_CTX_TABLE_HI, upper_32_bits(dev->ctx_table_dma));
	/* Keep the 32-bit hardware fence in step with the software sequence. */
	sgpu_wr(dev, SGPU_REG_FENCE, lower_32_bits(atomic64_read(&dev->completed)));
	sgpu_wr(dev, SGPU_REG_IRQ_MASK, SGPU_IRQ_FENCE | SGPU_IRQ_FAULT);
	sgpu_wr(dev, SGPU_REG_CTRL, SGPU_CTRL_ENABLE);
}

static void sgpu_complete_locked(struct sgpu_device *dev, struct list_head *done)
{
	u32 hw = sgpu_rd(dev, SGPU_REG_FENCE);
	struct sgpu_job *job, *tmp;
	bool progress = false;

	lockdep_assert_held(&dev->lock);

	list_for_each_entry_safe(job, tmp, &dev->jobs, node) {
		if ((s32)(hw - lower_32_bits(job->seq)) < 0)
			break;
		WRITE_ONCE(dev->ring_used, dev->ring_used - job->ncmds);
		atomic64_set(&dev->completed, job->seq);
		list_move_tail(&job->node, done);
		progress = true;
	}
	if (progress) {
		dev->last_progress = jiffies;
		wake_up_all(&dev->wq);
	}
}

static void sgpu_record_failure(struct sgpu_ctx *ctx, u64 fence, int err)
{
	ctx->failures[ctx->failure_next].fence = fence;
	ctx->failures[ctx->failure_next].err = err;
	ctx->failure_next = (ctx->failure_next + 1) % SGPU_MAX_FAILURES;
}

static void sgpu_fail_all_locked(struct sgpu_device *dev, struct list_head *done,
				 struct sgpu_ctx *guilty, int guilty_err,
				 int other_err)
{
	struct sgpu_job *job, *tmp;

	list_for_each_entry_safe(job, tmp, &dev->jobs, node) {
		sgpu_record_failure(job->ctx, job->seq,
				    job->ctx == guilty ? guilty_err : other_err);
		atomic64_set(&dev->completed, job->seq);
		list_move_tail(&job->node, done);
	}
	WRITE_ONCE(dev->ring_used, 0);
	dev->ring_tail = 0;
}

/*
 * Recover from a fault or hang: finished work is completed normally, the
 * responsible context is banned, and everything else in flight is cancelled.
 */
static void sgpu_reset_locked(struct sgpu_device *dev, struct list_head *done,
			      struct sgpu_ctx *guilty, int guilty_err)
{
	lockdep_assert_held(&dev->lock);

	sgpu_complete_locked(dev, done);
	if (guilty)
		guilty->banned = true;
	sgpu_fail_all_locked(dev, done, guilty, guilty_err, -ECANCELED);
	sgpu_hw_start(dev);
	dev->resets++;
	dev->last_progress = jiffies;
	wake_up_all(&dev->wq);
}

static irqreturn_t sgpu_irq_thread(int irq, void *data)
{
	struct sgpu_device *dev = data;
	u32 status = sgpu_rd(dev, SGPU_REG_IRQ_STATUS);
	LIST_HEAD(done);

	if (!status)
		return IRQ_NONE;
	sgpu_wr(dev, SGPU_REG_IRQ_STATUS, status);

	mutex_lock(&dev->lock);
	if (status & SGPU_IRQ_FAULT) {
		u32 slot = sgpu_rd(dev, SGPU_REG_FAULT_CTX);
		u32 code = sgpu_rd(dev, SGPU_REG_FAULT_CODE);
		struct sgpu_ctx *guilty = slot < SGPU_NUM_CTX ? dev->ctx[slot] : NULL;

		dev_warn_ratelimited(&dev->pdev->dev,
				     "fault code %u in context %u, resetting\n",
				     code, slot);
		sgpu_reset_locked(dev, &done, guilty, -EFAULT);
	} else {
		sgpu_complete_locked(dev, &done);
	}
	mutex_unlock(&dev->lock);

	sgpu_jobs_free(&done);
	return IRQ_HANDLED;
}

static void sgpu_watchdog(struct work_struct *work)
{
	struct sgpu_device *dev = container_of(to_delayed_work(work),
					       struct sgpu_device, watchdog);
	unsigned long timeout = msecs_to_jiffies(timeout_ms);
	LIST_HEAD(done);

	mutex_lock(&dev->lock);
	if (READ_ONCE(dev->dying) || list_empty(&dev->jobs)) {
		mutex_unlock(&dev->lock);
		return;
	}
	/* Blame the job actually stuck, not one whose interrupt is pending. */
	sgpu_complete_locked(dev, &done);
	if (!list_empty(&dev->jobs) &&
	    time_after(jiffies, dev->last_progress + timeout)) {
		struct sgpu_job *job = list_first_entry(&dev->jobs,
							struct sgpu_job, node);

		dev_warn_ratelimited(&dev->pdev->dev,
				     "hang in context %u, resetting\n",
				     job->ctx->slot);
		sgpu_reset_locked(dev, &done, job->ctx, -ETIMEDOUT);
	}
	if (!list_empty(&dev->jobs))
		schedule_delayed_work(&dev->watchdog, timeout / 2 + 1);
	mutex_unlock(&dev->lock);

	sgpu_jobs_free(&done);
}

/* Buffer objects */

static long sgpu_ioctl_bo_create(struct sgpu_ctx *ctx, void __user *uarg)
{
	struct sgpu_device *dev = ctx->dev;
	struct sgpu_bo_create args;
	struct sgpu_bo *bo;
	unsigned long first;
	u32 handle, i;
	long ret;

	if (copy_from_user(&args, uarg, sizeof(args)))
		return -EFAULT;
	if (args.pad || !args.size || args.size > SGPU_MAX_BO_SIZE)
		return -EINVAL;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	kref_init(&bo->ref);
	bo->ctx = ctx;
	bo->size = PAGE_ALIGN(args.size);
	bo->npages = bo->size >> SGPU_PAGE_SHIFT;

	ret = sgpu_enter(dev);
	if (ret)
		goto err_free;

	bo->vaddr = dma_alloc_coherent(&dev->pdev->dev, bo->size, &bo->dma,
				       GFP_KERNEL);
	if (!bo->vaddr) {
		ret = -ENOMEM;
		goto err_leave;
	}

	mutex_lock(&ctx->lock);
	/* One extra unmapped page keeps neighbouring buffers apart. */
	first = bitmap_find_next_zero_area(ctx->va_used, SGPU_VA_PAGES, 0,
					   bo->npages + 1, 0);
	if (first >= SGPU_VA_PAGES) {
		ret = -ENOSPC;
		goto err_unlock;
	}
	ret = xa_alloc(&ctx->bos, &handle, bo, XA_LIMIT(1, SGPU_MAX_HANDLES),
		       GFP_KERNEL);
	if (ret)
		goto err_unlock;

	bo->first_page = first;
	bitmap_set(ctx->va_used, first, bo->npages + 1);
	for (i = 0; i < bo->npages; i++) {
		ctx->page_bo[first + i] = bo;
		ctx->pt[first + i] = cpu_to_le64((bo->dma + (u64)i * SGPU_PAGE_SIZE) |
						 SGPU_PTE_VALID | SGPU_PTE_WRITE);
	}
	kref_get(&ctx->ref);
	mutex_unlock(&ctx->lock);
	sgpu_leave(dev);

	args.handle = handle;
	args.gpu_va = (u64)first << SGPU_PAGE_SHIFT;
	if (copy_to_user(uarg, &args, sizeof(args))) {
		/*
		 * User space never learned the handle. Dropping it here is
		 * safe even if another thread guessed it: xa_erase() decides
		 * which of the two owns the reference. remove_sem keeps the
		 * final free from racing with sgpu_remove().
		 */
		down_read(&dev->remove_sem);
		mutex_lock(&ctx->lock);
		bo = sgpu_bo_unpublish_locked(ctx, handle);
		mutex_unlock(&ctx->lock);
		if (bo)
			sgpu_bo_put(bo);
		up_read(&dev->remove_sem);
		return -EFAULT;
	}
	return 0;

err_unlock:
	mutex_unlock(&ctx->lock);
	dma_free_coherent(&dev->pdev->dev, bo->size, bo->vaddr, bo->dma);
err_leave:
	sgpu_leave(dev);
err_free:
	kfree(bo);
	return ret;
}

static long sgpu_ioctl_bo_destroy(struct sgpu_ctx *ctx, void __user *uarg)
{
	struct sgpu_bo_destroy args;
	struct sgpu_bo *bo;
	long ret;

	if (copy_from_user(&args, uarg, sizeof(args)))
		return -EFAULT;
	if (args.pad)
		return -EINVAL;

	ret = sgpu_enter(ctx->dev);
	if (ret)
		return ret;
	mutex_lock(&ctx->lock);
	bo = sgpu_bo_unpublish_locked(ctx, args.handle);
	mutex_unlock(&ctx->lock);
	if (bo)
		sgpu_bo_put(bo);
	sgpu_leave(ctx->dev);
	return bo ? 0 : -ENOENT;
}

static long sgpu_ioctl_bo_xfer(struct sgpu_ctx *ctx, void __user *uarg,
			       bool is_write)
{
	void __user *uptr;
	struct sgpu_bo_xfer args;
	struct sgpu_bo *bo;
	u64 end;
	void *bounce;
	long ret;

	if (copy_from_user(&args, uarg, sizeof(args)))
		return -EFAULT;
	if (args.pad || !args.len || args.len > SGPU_MAX_BO_SIZE ||
	    check_add_overflow(args.offset, args.len, &end))
		return -EINVAL;

	uptr = u64_to_user_ptr(args.user_ptr);
	bounce = kvmalloc(args.len, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;
	if (is_write && copy_from_user(bounce, uptr, args.len)) {
		ret = -EFAULT;
		goto out_free;
	}

	ret = sgpu_enter(ctx->dev);
	if (ret)
		goto out_free;
	mutex_lock(&ctx->lock);
	bo = xa_load(&ctx->bos, args.handle);
	if (bo)
		kref_get(&bo->ref);
	mutex_unlock(&ctx->lock);

	if (!bo)
		ret = -ENOENT;
	else if (end > bo->size)
		ret = -EINVAL;
	else if (is_write)
		memcpy(bo->vaddr + args.offset, bounce, args.len);
	else
		memcpy(bounce, bo->vaddr + args.offset, args.len);
	if (bo)
		sgpu_bo_put(bo);
	sgpu_leave(ctx->dev);

	if (!ret && !is_write && copy_to_user(uptr, bounce, args.len))
		ret = -EFAULT;
out_free:
	kvfree(bounce);
	return ret;
}

/* Command submission */

/*
 * Return the buffer that fully contains [va, va + len). Buffers are mapped
 * contiguously with an unmapped page between them, so it is enough that both
 * ends of the range land in the same buffer.
 */
static struct sgpu_bo *sgpu_lookup_range(struct sgpu_ctx *ctx, u64 va, u32 len)
{
	struct sgpu_bo *bo;
	u64 end;

	lockdep_assert_held(&ctx->lock);

	if (check_add_overflow(va, (u64)len - 1, &end) || end >= SGPU_VA_SIZE)
		return NULL;
	bo = ctx->page_bo[va >> SGPU_PAGE_SHIFT];
	if (!bo || ctx->page_bo[end >> SGPU_PAGE_SHIFT] != bo)
		return NULL;
	return bo;
}

static int sgpu_job_ref_range(struct sgpu_job *job, u64 va, u32 len)
{
	struct sgpu_bo *bo = sgpu_lookup_range(job->ctx, va, len);

	if (!bo)
		return -EINVAL;
	kref_get(&bo->ref);
	job->bos[job->nbos++] = bo;
	return 0;
}

/*
 * The only place that turns user commands into hardware commands. Opcodes are
 * allow-listed, addresses must lie inside this context's own buffers, and the
 * context field comes from the kernel, never from user space.
 */
static int sgpu_validate(struct sgpu_job *job, const struct sgpu_cmd *cmds)
{
	u32 slot = job->ctx->slot;
	u32 i;
	int ret;

	for (i = 0; i < job->ncmds; i++) {
		const struct sgpu_cmd *c = &cmds[i];
		struct sgpu_hw_cmd *hw = &job->cmds[i];
		u32 op;

		switch (c->opcode) {
		case SGPU_OP_NOP:
			if (c->len || c->src || c->dst)
				return -EINVAL;
			op = SGPU_HW_OP_NOP;
			break;
		case SGPU_OP_FILL:
			if (!c->len || c->len > SGPU_MAX_XFER || c->src > 0xff)
				return -EINVAL;
			ret = sgpu_job_ref_range(job, c->dst, c->len);
			if (ret)
				return ret;
			op = SGPU_HW_OP_FILL;
			break;
		case SGPU_OP_COPY:
			if (!c->len || c->len > SGPU_MAX_XFER)
				return -EINVAL;
			ret = sgpu_job_ref_range(job, c->src, c->len);
			if (ret)
				return ret;
			ret = sgpu_job_ref_range(job, c->dst, c->len);
			if (ret)
				return ret;
			op = SGPU_HW_OP_COPY;
			break;
		case SGPU_OP_TEST_HANG:
			if (!test_ops)
				return -EINVAL;
			op = SGPU_HW_OP_HANG;
			break;
		case SGPU_OP_TEST_FAULT:
			if (!test_ops)
				return -EINVAL;
			hw->opcode = cpu_to_le32(SGPU_HW_OP_FILL);
			hw->ctx = cpu_to_le32(slot);
			hw->src = 0;
			hw->dst = cpu_to_le64(SGPU_VA_SIZE);
			hw->len = cpu_to_le32(1);
			continue;
		default:
			return -EINVAL;
		}
		hw->opcode = cpu_to_le32(op);
		hw->ctx = cpu_to_le32(slot);
		hw->src = cpu_to_le64(c->src);
		hw->dst = cpu_to_le64(c->dst);
		hw->len = cpu_to_le32(c->len);
	}
	return 0;
}

static bool sgpu_ring_ready(struct sgpu_device *dev, u32 n)
{
	return READ_ONCE(dev->dying) ||
	       READ_ONCE(dev->ring_used) + n < SGPU_RING_ENTRIES;
}

static int sgpu_enqueue(struct sgpu_job *job, u64 *fence)
{
	struct sgpu_ctx *ctx = job->ctx;
	struct sgpu_device *dev = ctx->dev;
	u32 i, tail;
	long t;

	for (;;) {
		mutex_lock(&dev->lock);
		if (READ_ONCE(dev->dying)) {
			mutex_unlock(&dev->lock);
			return -ENODEV;
		}
		if (ctx->banned) {
			mutex_unlock(&dev->lock);
			return -EIO;
		}
		if (dev->ring_used + job->ncmds < SGPU_RING_ENTRIES)
			break;
		mutex_unlock(&dev->lock);

		t = wait_event_interruptible_timeout(dev->wq,
				sgpu_ring_ready(dev, job->ncmds), SGPU_RING_WAIT);
		if (t < 0)
			return t;
		if (!t)
			return -EBUSY;
	}

	job->seq = ++dev->next_seq;
	/* Only the last command of a job may complete its fence. */
	for (i = 0; i < job->ncmds; i++) {
		u64 f = i + 1 == job->ncmds ? job->seq : job->seq - 1;

		job->cmds[i].fence = cpu_to_le32(lower_32_bits(f));
		dev->ring[(dev->ring_tail + i) % SGPU_RING_ENTRIES] = job->cmds[i];
	}
	tail = (dev->ring_tail + job->ncmds) % SGPU_RING_ENTRIES;
	dev->ring_tail = tail;
	WRITE_ONCE(dev->ring_used, dev->ring_used + job->ncmds);

	if (list_empty(&dev->jobs)) {
		dev->last_progress = jiffies;
		schedule_delayed_work(&dev->watchdog,
				      msecs_to_jiffies(timeout_ms) / 2 + 1);
	}
	list_add_tail(&job->node, &dev->jobs);
	ctx->last_fence = job->seq;
	*fence = job->seq;

	/* writel() orders this after the ring writes above. */
	sgpu_wr(dev, SGPU_REG_RING_TAIL, tail);
	mutex_unlock(&dev->lock);
	return 0;
}

static long sgpu_ioctl_submit(struct sgpu_ctx *ctx, void __user *uarg)
{
	struct sgpu_device *dev = ctx->dev;
	struct sgpu_submit args;
	struct sgpu_cmd *cmds;
	struct sgpu_job *job;
	long ret;

	if (copy_from_user(&args, uarg, sizeof(args)))
		return -EFAULT;
	if (args.pad || !args.count || args.count > SGPU_MAX_CMDS)
		return -EINVAL;

	cmds = memdup_array_user(u64_to_user_ptr(args.cmds_ptr), args.count,
				 sizeof(*cmds));
	if (IS_ERR(cmds))
		return PTR_ERR(cmds);

	/* A COPY references two buffers, so at most two per command. */
	job = kzalloc(struct_size(job, bos, 2 * args.count), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out_cmds;
	}
	job->cmds = kcalloc(args.count, sizeof(*job->cmds), GFP_KERNEL);
	if (!job->cmds) {
		kfree(job);
		ret = -ENOMEM;
		goto out_cmds;
	}
	kref_get(&ctx->ref);
	job->ctx = ctx;
	job->ncmds = args.count;

	ret = sgpu_enter(dev);
	if (ret) {
		sgpu_job_free(job);	/* holds no buffers yet */
		goto out_cmds;
	}
	mutex_lock(&ctx->lock);
	ret = sgpu_validate(job, cmds);
	mutex_unlock(&ctx->lock);
	if (!ret)
		ret = sgpu_enqueue(job, &args.fence);
	/* Freed inside remove_sem: this can drop the last buffer reference. */
	if (ret)
		sgpu_job_free(job);
	sgpu_leave(dev);
	if (ret)
		goto out_cmds;

	/* The job now belongs to the device and may already be freed. */
	kfree(cmds);
	return copy_to_user(uarg, &args, sizeof(args)) ? -EFAULT : 0;

out_cmds:
	kfree(cmds);
	return ret;
}

static long sgpu_ioctl_wait(struct sgpu_ctx *ctx, void __user *uarg)
{
	struct sgpu_device *dev = ctx->dev;
	struct sgpu_wait args;
	u64 last;
	long t;
	u32 i;

	if (copy_from_user(&args, uarg, sizeof(args)))
		return -EFAULT;
	if (args.timeout_ms > SGPU_MAX_WAIT_MS)
		return -EINVAL;

	/* A context may only wait on fences that it created itself. */
	mutex_lock(&dev->lock);
	last = ctx->last_fence;
	mutex_unlock(&dev->lock);
	if (!args.fence || args.fence > last)
		return -EINVAL;

	t = wait_event_interruptible_timeout(dev->wq,
			atomic64_read(&dev->completed) >= args.fence,
			msecs_to_jiffies(args.timeout_ms));
	if (t < 0)
		return t;
	if (atomic64_read(&dev->completed) < args.fence)
		return -ETIME;

	args.status = 0;
	mutex_lock(&dev->lock);
	for (i = 0; i < SGPU_MAX_FAILURES; i++)
		if (ctx->failures[i].fence == args.fence)
			args.status = ctx->failures[i].err;
	mutex_unlock(&dev->lock);

	return copy_to_user(uarg, &args, sizeof(args)) ? -EFAULT : 0;
}

static long sgpu_ioctl_info(struct sgpu_ctx *ctx, void __user *uarg)
{
	struct sgpu_device *dev = ctx->dev;
	struct sgpu_info info = {
		.version = SGPU_VERSION_VALUE,
		.num_contexts = SGPU_NUM_CTX,
		.max_bo_size = SGPU_MAX_BO_SIZE,
		.context_id = ctx->slot,
		.test_ops = test_ops,
	};

	mutex_lock(&dev->lock);
	info.banned = ctx->banned;
	info.resets = dev->resets;
	mutex_unlock(&dev->lock);

	return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
}

static long sgpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct sgpu_ctx *ctx = filp->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case SGPU_IOC_INFO:
		return sgpu_ioctl_info(ctx, uarg);
	case SGPU_IOC_BO_CREATE:
		return sgpu_ioctl_bo_create(ctx, uarg);
	case SGPU_IOC_BO_DESTROY:
		return sgpu_ioctl_bo_destroy(ctx, uarg);
	case SGPU_IOC_BO_WRITE:
		return sgpu_ioctl_bo_xfer(ctx, uarg, true);
	case SGPU_IOC_BO_READ:
		return sgpu_ioctl_bo_xfer(ctx, uarg, false);
	case SGPU_IOC_SUBMIT:
		return sgpu_ioctl_submit(ctx, uarg);
	case SGPU_IOC_WAIT:
		return sgpu_ioctl_wait(ctx, uarg);
	default:
		return -ENOTTY;
	}
}

/* File operations */

static int sgpu_open(struct inode *inode, struct file *filp)
{
	struct sgpu_device *dev = container_of(filp->private_data,
					       struct sgpu_device, misc);
	struct sgpu_ctx *ctx;
	int slot, ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	kref_init(&ctx->ref);
	mutex_init(&ctx->lock);
	xa_init_flags(&ctx->bos, XA_FLAGS_ALLOC1);
	ctx->dev = dev;

	ret = sgpu_enter(dev);
	if (ret)
		goto err_free;
	ctx->pt = dma_alloc_coherent(&dev->pdev->dev,
				     SGPU_VA_PAGES * sizeof(*ctx->pt),
				     &ctx->pt_dma, GFP_KERNEL);
	if (!ctx->pt) {
		ret = -ENOMEM;
		goto err_leave;
	}

	mutex_lock(&dev->lock);
	for (slot = 0; slot < SGPU_NUM_CTX; slot++)
		if (!dev->ctx[slot])
			break;
	if (slot == SGPU_NUM_CTX) {
		mutex_unlock(&dev->lock);
		ret = -EBUSY;
		goto err_pt;
	}
	ctx->slot = slot;
	dev->ctx[slot] = ctx;
	dev->ctx_table[slot] = cpu_to_le64(ctx->pt_dma);
	mutex_unlock(&dev->lock);

	kref_get(&dev->ref);
	sgpu_leave(dev);
	filp->private_data = ctx;
	return 0;

err_pt:
	dma_free_coherent(&dev->pdev->dev, SGPU_VA_PAGES * sizeof(*ctx->pt),
			  ctx->pt, ctx->pt_dma);
err_leave:
	sgpu_leave(dev);
err_free:
	mutex_destroy(&ctx->lock);
	kfree(ctx);
	return ret;
}

static int sgpu_release(struct inode *inode, struct file *filp)
{
	struct sgpu_ctx *ctx = filp->private_data;
	struct sgpu_device *dev = ctx->dev;
	struct sgpu_bo *bo;
	unsigned long handle;

	/*
	 * After a hot-unplug the final context reference can also be the final
	 * device reference, and remove_sem lives inside the device. Keep the
	 * device alive until the semaphore has been released.
	 */
	kref_get(&dev->ref);

	/* Excludes sgpu_remove() so it can revoke DMA memory safely. */
	down_read(&dev->remove_sem);
	for (;;) {
		handle = 0;
		mutex_lock(&ctx->lock);
		bo = xa_find(&ctx->bos, &handle, ULONG_MAX, XA_PRESENT);
		if (bo)
			sgpu_bo_unpublish_locked(ctx, handle);
		mutex_unlock(&ctx->lock);
		if (!bo)
			break;
		sgpu_bo_put(bo);
	}
	sgpu_ctx_put(ctx);
	up_read(&dev->remove_sem);
	kref_put(&dev->ref, sgpu_dev_release);
	return 0;
}

static const struct file_operations sgpu_fops = {
	.owner		= THIS_MODULE,
	.open		= sgpu_open,
	.release	= sgpu_release,
	.unlocked_ioctl	= sgpu_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

/* Debugfs */

/*
 * Report device state for debugging. Context pointers are only dereferenced
 * under dev->lock: sgpu_ctx_release() clears the slot under the same lock
 * before freeing, so a status read cannot see a context being torn down.
 * Unlocked access to objects with independent lifetimes in a status path is a
 * classic source of use-after-free. debugfs_remove_recursive() in
 * sgpu_remove() waits for running readers before anything is freed.
 */
static int sgpu_status_show(struct seq_file *m, void *unused)
{
	struct sgpu_device *dev = m->private;
	struct sgpu_job *job;
	u32 jobs = 0;
	int slot;

	mutex_lock(&dev->lock);
	list_for_each_entry(job, &dev->jobs, node)
		jobs++;
	seq_printf(m, "completed: %lld\nnext_seq: %llu\njobs: %u\nring_used: %u\nresets: %u\n",
		   (long long)atomic64_read(&dev->completed), dev->next_seq,
		   jobs, dev->ring_used, dev->resets);
	for (slot = 0; slot < SGPU_NUM_CTX; slot++) {
		struct sgpu_ctx *ctx = dev->ctx[slot];

		if (ctx)
			seq_printf(m, "context %d: banned=%d last_fence=%llu\n",
				   slot, ctx->banned, ctx->last_fence);
	}
	mutex_unlock(&dev->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgpu_status);

/* PCI */

static int sgpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct sgpu_device *dev;
	int ret;

	BUILD_BUG_ON(sizeof(struct sgpu_hw_cmd) != SGPU_HW_CMD_SIZE);
	BUILD_BUG_ON(PAGE_SIZE != SGPU_PAGE_SIZE);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	kref_init(&dev->ref);
	dev->pdev = pdev;
	mutex_init(&dev->lock);
	init_rwsem(&dev->remove_sem);
	init_waitqueue_head(&dev->wq);
	INIT_LIST_HEAD(&dev->jobs);
	INIT_DELAYED_WORK(&dev->watchdog, sgpu_watchdog);
	atomic64_set(&dev->completed, 0);
	pci_set_drvdata(pdev, dev);

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_free;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto err_disable;
	ret = pci_request_regions(pdev, "sgpu");
	if (ret)
		goto err_disable;
	dev->regs = pci_iomap(pdev, 0, SGPU_BAR_SIZE);
	if (!dev->regs) {
		ret = -ENOMEM;
		goto err_regions;
	}
	if (sgpu_rd(dev, SGPU_REG_ID) != SGPU_ID_VALUE ||
	    sgpu_rd(dev, SGPU_REG_VERSION) != SGPU_VERSION_VALUE) {
		dev_err(&pdev->dev, "unsupported device\n");
		ret = -ENODEV;
		goto err_unmap;
	}
	pci_set_master(pdev);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0)
		goto err_master;

	dev->ring = dma_alloc_coherent(&pdev->dev,
				       SGPU_RING_ENTRIES * sizeof(*dev->ring),
				       &dev->ring_dma, GFP_KERNEL);
	if (!dev->ring) {
		ret = -ENOMEM;
		goto err_vectors;
	}
	dev->ctx_table = dma_alloc_coherent(&pdev->dev,
					    SGPU_NUM_CTX * sizeof(*dev->ctx_table),
					    &dev->ctx_table_dma, GFP_KERNEL);
	if (!dev->ctx_table) {
		ret = -ENOMEM;
		goto err_ring;
	}

	sgpu_hw_start(dev);

	ret = request_threaded_irq(pci_irq_vector(pdev, 0), NULL, sgpu_irq_thread,
				   IRQF_ONESHOT, "sgpu", dev);
	if (ret)
		goto err_hw;

	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = "sgpu";
	dev->misc.fops = &sgpu_fops;
	dev->misc.parent = &pdev->dev;
	dev->misc.mode = 0666;
	ret = misc_register(&dev->misc);
	if (ret)
		goto err_irq;

	dev->debugfs = debugfs_create_dir("sgpu", NULL);
	debugfs_create_file("status", 0400, dev->debugfs, dev, &sgpu_status_fops);

	dev_info(&pdev->dev, "sgpu ready, %d contexts\n", SGPU_NUM_CTX);
	return 0;

err_irq:
	free_irq(pci_irq_vector(pdev, 0), dev);
err_hw:
	sgpu_wr(dev, SGPU_REG_CTRL, SGPU_CTRL_RESET);
	dma_free_coherent(&pdev->dev, SGPU_NUM_CTX * sizeof(*dev->ctx_table),
			  dev->ctx_table, dev->ctx_table_dma);
err_ring:
	dma_free_coherent(&pdev->dev, SGPU_RING_ENTRIES * sizeof(*dev->ring),
			  dev->ring, dev->ring_dma);
err_vectors:
	pci_free_irq_vectors(pdev);
err_master:
	pci_clear_master(pdev);
err_unmap:
	pci_iounmap(pdev, dev->regs);
err_regions:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
err_free:
	kref_put(&dev->ref, sgpu_dev_release);
	return ret;
}

/* Free the DMA memory of a context that user space still holds open. */
static void sgpu_ctx_revoke(struct sgpu_ctx *ctx)
{
	struct device *d = &ctx->dev->pdev->dev;
	struct sgpu_bo *bo;
	unsigned long handle;

	mutex_lock(&ctx->lock);
	xa_for_each(&ctx->bos, handle, bo) {
		dma_free_coherent(d, bo->size, bo->vaddr, bo->dma);
		bo->vaddr = NULL;
	}
	dma_free_coherent(d, SGPU_VA_PAGES * sizeof(*ctx->pt), ctx->pt, ctx->pt_dma);
	ctx->pt = NULL;
	mutex_unlock(&ctx->lock);
}

static void sgpu_remove(struct pci_dev *pdev)
{
	struct sgpu_device *dev = pci_get_drvdata(pdev);
	LIST_HEAD(done);
	int slot;

	debugfs_remove_recursive(dev->debugfs);
	misc_deregister(&dev->misc);

	WRITE_ONCE(dev->dying, true);
	wake_up_all(&dev->wq);
	down_write(&dev->remove_sem);

	free_irq(pci_irq_vector(pdev, 0), dev);
	cancel_delayed_work_sync(&dev->watchdog);

	mutex_lock(&dev->lock);
	sgpu_wr(dev, SGPU_REG_CTRL, SGPU_CTRL_RESET);
	sgpu_fail_all_locked(dev, &done, NULL, 0, -ENODEV);
	dev->gone = true;
	mutex_unlock(&dev->lock);
	wake_up_all(&dev->wq);

	/* The device has stopped, so no job can reference these any more. */
	sgpu_jobs_free(&done);
	for (slot = 0; slot < SGPU_NUM_CTX; slot++)
		if (dev->ctx[slot])
			sgpu_ctx_revoke(dev->ctx[slot]);
	up_write(&dev->remove_sem);

	dma_free_coherent(&pdev->dev, SGPU_NUM_CTX * sizeof(*dev->ctx_table),
			  dev->ctx_table, dev->ctx_table_dma);
	dma_free_coherent(&pdev->dev, SGPU_RING_ENTRIES * sizeof(*dev->ring),
			  dev->ring, dev->ring_dma);
	pci_free_irq_vectors(pdev);
	pci_clear_master(pdev);
	pci_iounmap(pdev, dev->regs);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	kref_put(&dev->ref, sgpu_dev_release);
}

static const struct pci_device_id sgpu_ids[] = {
	{ PCI_DEVICE(SGPU_PCI_VENDOR, SGPU_PCI_DEVICE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, sgpu_ids);

static struct pci_driver sgpu_driver = {
	.name		= "sgpu",
	.id_table	= sgpu_ids,
	.probe		= sgpu_probe,
	.remove		= sgpu_remove,
};
module_pci_driver(sgpu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for the SGPU virtual command processor");
