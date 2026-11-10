# Case study: a refcount race that syzkaller found and unit tests could not

syzkaller found this bug in the driver's own submission path. Raw reports, the
fuzzing configuration and the unfixed source are in
`evidence/bug-002-fuzz-refcount-uaf/`.

## Symptom

The bug appeared in the first hour-long run after the fuzzing setup worked. Three
VMs hit the same pair of reports, each pair within the same crash log:

```text
refcount_t: addition on 0; use-after-free.
WARNING: CPU: 1 PID: 4212 at lib/refcount.c:25 refcount_warn_saturate
 kref_get include/linux/kref.h:45
 sgpu_job_ref_range driver/sgpu.c:586
 sgpu_validate driver/sgpu.c:616
 sgpu_ioctl_submit driver/sgpu.c:760

BUG: KASAN: slab-use-after-free in sgpu_jobs_free+0x244/0x430 [sgpu]
Write of size 4 at addr ff11000002fde400 by task irq/24-sgpu/192
```

## Reading the reports

| Evidence | Conclusion |
|---|---|
| The refcount warning is `addition on 0`, under `kref_get` in `sgpu_job_ref_range` | Submit took a reference on a buffer whose count had already reached zero |
| Register `R12` in the warning and the KASAN object are both `ff11000002fde400` | Both reports concern one object, and `kref` is the first field of `struct sgpu_bo` |
| KASAN says the access is 0 bytes inside a 64-byte object, a write of size 4 | That is the reference count itself, written by a later `kref_put` |
| KASAN freed-by stack: `kfree`, `sgpu_bo_release`, `sgpu_jobs_free`, `sgpu_watchdog` | The last reference was dropped by the watchdog worker, outside any ioctl |
| KASAN access stack: `sgpu_jobs_free`, `sgpu_irq_thread` | A later job completion dropped a reference it should never have held |
| Console, just before: `hang in context 2, resetting` | The watchdog reset path was running, which is what frees jobs from a worker |

## Root cause

The per-context table `page_bo[]` maps a device address to its buffer. Entries
were only cleared in `sgpu_bo_release()`, which runs after the count reaches zero
and must first take `ctx->lock`. That leaves a window:

| Step | Watchdog worker | Submit ioctl |
|---|---|---|
| 1 | | Holds `ctx->lock`, validating commands |
| 2 | Drops the job's reference, which is the last one, because user space already destroyed the handle | |
| 3 | `sgpu_bo_release()` blocks on `ctx->lock` | |
| 4 | | `page_bo[va]` still names the buffer. `kref_get()` on a count of 0 |
| 5 | | The job stores the pointer, unlocks |
| 6 | Takes the lock, clears the entry, `kfree(bo)` | |
| 7 | | The new job is now holding a freed buffer. Its completion writes to freed memory |

The device would also be handed the address of freed DMA memory. `/dev/sgpu` is
open to any local user. The fuzzer's crash went through the reset path, using a
hang command that needs `test_ops=1`, but from reading the code, ordinary job
completion in the interrupt thread drops the last reference the same way. So this
was a memory-safety bug reachable from an unprivileged process, without test
commands. The no-test-commands path was not demonstrated.

The handle table was correct: it owned a reference, so finding a buffer there
proved the buffer was alive. `page_bo[]` looked like the same kind of table but
owned nothing.

## Why the existing tests missed it

The unit suite had `test_destroy_in_flight`, which destroys buffers while jobs are
running, and a stress test. Neither submitted to an address **after** its handle
was gone, because a well-behaved client has no reason to, so the suite never
tested that kind of misuse. The fuzzer did, since it treats the address as a value
it can reuse after a destroy. The reset path made the window wider by freeing jobs
from a worker.

## Fix

A lookup entry now has the lifetime of the handle whose reference it borrows, and
both are removed in the same critical section:

```c
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
```

Every path that removes a handle uses this helper. The invariant is now: a
non-NULL `page_bo[]` entry, read under `ctx->lock`, means the handle's reference
still exists, so `kref_get()` cannot see zero. The page table itself stays valid
until release, so jobs already queued finish normally.

`kref_get_unless_zero()` in the lookup would have stopped the crash too, but would
have left a logic bug: a destroyed buffer could still be targeted by new commands
for as long as an old job kept it alive.

## Verification

- `test_destroy_unpublishes_va` makes this deterministic: a hang holds a job in
  flight, the buffer is destroyed, and a new submit to its address must return
  `-EINVAL`. It passes on the fixed driver.
- The `unpublish_va` mutant removes the fix, and the suite catches it: the submit
  is accepted instead of returning `-EINVAL`.
- syzkaller did not produce a reproducer (`repro=false`), which fits a
  timing-dependent race. So the deterministic test covers the stale lookup.
- Before the fix, the crash appeared in 3 VMs during 16,765 executed programs, the
  first after about 12 minutes. A one-hour rerun of 30,170 programs after the fix
  had no crash. That is supporting evidence, not proof.

Full results are in `docs/test-record.md`.

## Lesson

Fix the invariant, not the symptom. A pointer table must either own a reference or
have exactly the same lifetime as something that does.
