# SGPU driver threat model

`/dev/sgpu` is deliberately world-accessible (mode 0666), the way render nodes
are on many Linux systems. The driver is therefore written on the assumption
that any local user may call it with hostile input.

## Assets

- Kernel memory integrity and confidentiality.
- Host memory reachable through device DMA.
- Isolation between GPU contexts belonging to different users.
- Availability of the device for other contexts.

## Trust boundaries

| Boundary | Trust decision |
|---|---|
| User space to ioctl | Fully untrusted. Every field is validated |
| User command stream to ring | Untrusted. Rebuilt by the kernel, never copied through |
| Context to context | Mutually distrusting |
| Device to driver | Trusted in this model. See "Out of scope" |

## Threats and mitigations

| # | Threat | Mitigation | Verified by |
|---|---|---|---|
| T1 | A command makes the device read or write another context or kernel memory | `sgpu_validate()` requires every range to lie inside a buffer of the calling context. Addresses are only ever per-context VAs | `test_validator`, `test_isolation` |
| T2 | Integer overflow in `va + len` or `offset + len` bypasses a bounds check | `check_add_overflow()` on every range | `UINT64_MAX` cases in tests, syzkaller raw variants |
| T3 | User space selects another context's page table through the command's context field | The field does not exist in the user ABI. The kernel fills it in | ABI design, `test_isolation` |
| T4 | A buffer is freed while a queued job still references it, giving the device a use-after-free | Each job holds a reference on every buffer it uses. Page table entries stay valid until the last reference is gone | `test_destroy_in_flight`, `test_close_in_flight`, stress under KASAN |
| T5 | An overrun off the end of one buffer reaches the next | One unmapped guard page after each buffer, and a validator that rejects ranges crossing a buffer boundary | `test_guard_pages`, `test_validator` |
| T6 | Handles from one context are used in another | Handles live in a per-context xarray | `test_isolation` |
| T7 | A context observes another context's progress through fences | `WAIT` rejects any fence newer than the context's own last fence | `test_fences` |
| T8 | A context hangs or faults the device to deny service to others | Watchdog plus fault interrupt trigger a reset. Only the responsible context is banned. Innocent in-flight jobs report `-ECANCELED`, and the device keeps serving them | `test_hang_recovery`, `test_fault_recovery` |
| T9 | A slow user-memory access stalls the driver for everybody | All user copies happen outside locks through bounce buffers | Code review; no proof-of-concept was written |
| T10 | The device is removed while a context is open, leaving dangling DMA or MMIO | `remove_sem` fences ioctls from removal. DMA memory is revoked and later calls return `-ENODEV` | `--unplug` test under KASAN |
| T11 | Kernel memory leaks to user space through uninitialised structure padding | Output structures are fully initialised, and padding is required to be zero on input | Code review, tests on `pad` |
| T12 | Test-only commands are reachable in production | Rejected unless the module is loaded with `test_ops=1` by root | `test_test_ops_disabled` |
| T13 | A submit reaches a destroyed buffer through its address while a job keeps the memory alive, or while the last reference is being dropped | The address lookup entries are removed in the same critical section as the handle | `test_destroy_unpublishes_va`, found by syzkaller, see `case-study-fuzz-refcount-uaf.md` |

## Residual risks

- A single user can open all 16 contexts and exhaust them. A real driver would
  apply per-user limits or cgroup accounting.
- Each context can pin at most about 4 MiB of coherent DMA memory, but that is
  not charged to the user's memory cgroup.
- The stored failure history is limited to 64 entries per context. A very old
  failed fence reports status 0 once it has been evicted.
- A context can keep the ring busy. There is no scheduler, so submissions are
  processed in arrival order with no fairness between contexts.

## Out of scope

- A malicious device. Register values such as the fence are used without
  hardening against a hostile device, and no IOMMU is configured in the test
  setup. Confidential-computing threat models would need both.
- Side channels between contexts.
- Physical attacks and DMA from other devices.
