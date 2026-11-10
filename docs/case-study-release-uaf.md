# Case study: use-after-free in the close path after hot-unplug

A bug in this project's own driver, found by its own test run. Raw logs and the
unfixed source are in `evidence/bug-001-release-uaf/`.

## Symptom

The hot-unplug test (`test_sgpu --unplug`) ended, and the kernel log then showed
two reports from the same task, 34 ms apart:

```text
WARNING: held lock freed!
test_sgpu/468 is freeing memory ff11000005189800-ff11000005189bff, with a lock still held there!
ff110000051899d8 (&dev->remove_sem){++++}-{3:3}, at: sgpu_release+0xd6/0x270 [sgpu]

BUG: KASAN: slab-use-after-free in __up_read+0x576/0x770
Read of size 8 at addr ff110000051899d0 by task test_sgpu/468
```

## Reading the reports

| Evidence | Conclusion |
|---|---|
| lockdep: memory `...9800-...9bff` is freed with `remove_sem` held inside it | The freed object contains the semaphore, so it is `struct sgpu_device` |
| lockdep backtrace: `kfree` from `sgpu_ctx_release` from `sgpu_release` | The free happens on the last context reference, inside the file release path |
| KASAN: read at `...99d0`, inside the freed range | The access is on the same object. It is the `up_read()` that follows |
| KASAN call trace: `__up_read` from `sgpu_release` | The access happens on the way out of the same function |
| KASAN allocation stack: `sgpu_probe` | Confirms the object is the per-device structure |
| `__fput` from `do_exit` | The file was closed at process exit, after the device had been removed |

## Root cause

The buggy sequence in `sgpu_release()`:

```c
down_read(&dev->remove_sem);
/* ... drop every buffer ... */
sgpu_ctx_put(ctx);		/* may free dev */
up_read(&dev->remove_sem);	/* dev may already be gone */
```

`sgpu_device` is reference counted, with one reference from the PCI binding and
one from each context. In normal operation the PCI binding outlives every file,
so `sgpu_ctx_put()` never drops the last device reference. After a hot-unplug
that binding is gone, and the last context to close holds the final reference.
`sgpu_ctx_put()` then frees the device, including the semaphore that the caller
still has to release.

The driver had carefully protected `sgpu_remove()` against racing with ioctls,
but the semaphore doing that protection was itself inside an object whose
lifetime the caller was ending.

## Fix

Pin the device for the duration of the function, so the semaphore outlives its
release:

```c
kref_get(&dev->ref);
down_read(&dev->remove_sem);
/* ... */
sgpu_ctx_put(ctx);
up_read(&dev->remove_sem);
kref_put(&dev->ref, sgpu_dev_release);
```

## Why normal testing missed it

The functional, isolation and stress tests all close files while the device is
still bound, so this path never drops the last device reference. It needed a
specific order of events: remove the device, then close the file. That is why
hot-unplug was given its own test instead of being assumed to work.

## Verification

The same test run after the fix is recorded in `docs/test-record.md`.

## Lesson

A lock must not live inside an object whose final reference can be dropped while
that lock is held. When tearing down reference-counted objects, check what the
code does after each `put`, not only what it does before it.
