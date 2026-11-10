# Source analysis of ALSA sequencer fix 60adcfde

**Scope: this is a reading of the source and the public patch only. The bug was
not reproduced, and no KASAN report from this bug was produced in this project.**

| | |
|---|---|
| Commit | `60adcfde92fa40fcb2dbf7cc52f9b096e0cd109a` |
| Subject | ALSA: seq: Fix racy access for queue timer in proc read |
| Author | Takashi Iwai |
| Reported by | syzbot |
| File | `sound/core/seq/seq_timer.c`, function `snd_seq_info_timer_read()` |
| Parent read for this analysis | `5d1b71226dc4`, Linux 5.4.0-rc8 tree |

## Root cause

`snd_seq_info_timer_read()` prints the state of each sequencer queue's timer
into `/proc/asound/seq/timer`. Before the fix it held only the queue's use
reference, obtained through `queueptr()`.

That reference protects the wrong object. It keeps `struct snd_seq_queue` alive,
but the timer instance reachable through `q->timer->timeri` has a shorter
lifetime of its own: `snd_seq_timer_close()` detaches it and frees it while the
queue continues to exist. In the paths I read (`snd_seq_queue_use()` and
`snd_seq_ioctl_set_queue_timer()`), that close happens under `q->timer_mutex`.

The reader did not take `q->timer_mutex`, so nothing ordered it against the
close path. It could obtain the `ti` pointer and then dereference it after the
instance had been released, which is a use-after-free.

```text
before the fix
  queueptr(idx)                     protects the queue object only
  tmr = q->timer
  ti  = tmr->timeri                 no lock shared with the close path
  use ti, tmr->...

after the fix
  queueptr(idx)
  mutex_lock(&q->timer_mutex)       same lock as the close path
  tmr = q->timer;   if (!tmr) goto unlock
  ti  = tmr->timeri; if (!ti)  goto unlock
  use ti, tmr->...
unlock:
  mutex_unlock(&q->timer_mutex)
  queuefree(q)
```

## Why the fix is correct

1. The reader and the close path now use the same mutex, so `ti` cannot be freed
   between being read and being used.
2. A mutex is acceptable here because a proc read runs in process context and
   is allowed to sleep.
3. Replacing `continue` with `goto unlock` gives every exit path a single unlock
   before the existing `queuefree(q)`, in the reverse order of acquisition.

The change is small (9 insertions, 5 deletions) and does not alter the proc
output format.

## Bug class

A reference on a container does not protect objects the container points to
when those objects can be replaced or freed independently. Debug and status
paths are prone to this because they are rarely exercised and rarely reviewed
alongside teardown code.

## How this influenced the SGPU driver

`sgpu_status_show()` is the equivalent status path in this project. It walks
every context and is written so this bug class cannot occur:

- It dereferences a context only under `dev->lock`.
- `sgpu_ctx_release()` clears the slot under that same lock before freeing.
- `debugfs_remove_recursive()` in `sgpu_remove()` waits for running readers.

`test_status_concurrent` and the hot-unplug test exercise these paths together
under KASAN, lockdep and KCSAN.

## Reading a KASAN report

These are the fields I use to go from a report to a root cause.

| Part of the report | What it answers |
|---|---|
| `BUG: KASAN: <type> in <function>` | The bug class, such as `slab-use-after-free` or `slab-out-of-bounds`, and the faulting function |
| `Read/Write of size N at addr ... by task ...` | Access direction, width and the task responsible |
| First call trace | The path of the bad access. The top frames show where, the lower ones how it was reached |
| `Allocated by task` | Where the object was created, which identifies its owner |
| `Freed by task` | Who released it. For a use-after-free, compare this path with the access path |
| `The buggy address belongs to the object at ...` | Object cache, size and offset. The offset can be mapped to a field with `pahole` |
| Memory state around the address | Shadow bytes: `fb` freed, `fc` redzone, partial values for partly addressable granules |

To convert frames to source lines, use `scripts/decode_stacktrace.sh` with a
`vmlinux` that has debug info. For a use-after-free the useful question is
which lock or reference was supposed to order the free path against the access
path, and that is exactly the question that exposes the missing mutex above.
