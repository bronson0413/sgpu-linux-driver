# Test record

Every number here comes from a log in `evidence/results/`, unless a section names a
different directory. A run that turned out to be invalid is listed as invalid and
not counted.

## Environment

| Item | Value |
|---|---|
| Host | WSL2 Ubuntu, 8 logical CPUs, 15 GiB RAM |
| Kernel | Linux 6.12 |
| Main test kernel | KASAN (inline), KCOV, PROVE_LOCKING, PROVE_RCU, UBSAN |
| Data-race kernel | KCSAN |
| VM | QEMU 10.2.1 with the `sgpu` device, TCG without KVM, 2 GiB, 4 vCPUs (fuzzing: 2 vCPUs per VM) |
| Driver under test | `driver/sgpu.c` after both fixes. The file was unchanged across every run below except the before-fix fuzzing column |

TCG and the debug options make everything slow. Rates below are not performance claims.

## Functional, adversarial and stress: `vmtest-20261110-061700.log`

Result: `RESULT: PASS`, `boot_hits=0`, `dmesg_hits=0`, `debug_locks=1`, `taint=4096`
(out-of-tree module only). The scan covers KASAN, UBSAN, BUG, WARNING, Oops and
lockdep reports.

| Step | Result |
|---|---|
| mychar with 60 s stress, 4 threads | 142 checks pass, 1,939,520 operations, 0 unexpected |
| mychar repeat run (proves tests clean up after themselves) | 137 checks pass |
| mychar reload ×5 | pass |
| sgpu with test commands and 60 s stress, 4 threads | 292 checks pass, 90,766 jobs, 0 unexpected |
| debugfs status reader racing context churn | 10,536 reads against 4,181 context cycles |
| sgpu repeat run | 291 checks pass |
| sgpu without test commands | 276 checks pass |
| Hot-unplug with a context open and a job outstanding | 5 checks pass |
| Rescan and full suite after unplug | 276 checks pass |

The check count differs between runs because some cases only exist with
`test_ops=1`, and a few are specific to the first run.

## Bugs found

| Bug | Found by | Evidence | Write-up |
|---|---|---|---|
| Use-after-free and held-lock-freed in `sgpu_release()` | Hot-unplug test, KASAN and lockdep (`vmtest-20261110-012302`) | `evidence/bug-001-release-uaf/` | [case study](case-study-release-uaf.md) |
| Refcount-on-zero and use-after-free through a stale `page_bo[]` entry | syzkaller, KASAN (`fuzz-20261110-034313`) | `evidence/bug-002-fuzz-refcount-uaf/` | [case study](case-study-fuzz-refcount-uaf.md) |

The first bug's run failed. The test above is the same suite after both fixes.

## Data races: `kcsan-20261110-060758.log`

| Workload, 120 s each, 4 threads | Result |
|---|---|
| mychar | 142 checks pass, 13,188,917 operations |
| sgpu | 292 checks pass, 569,255 jobs |
| Hot-unplug | 5 checks pass |

KCSAN printed 5 reports, none with a racing access in either driver. All 5 are in
the kernel's own optimistic spinning code (`osq_lock` and `osq_unlock`).
`kcsan-classified.txt` is the output of `tools/fuzzlog.py`, which classifies a
report by the frame directly under each access line, not by whether a driver
appears somewhere in the call stack. That rule has a unit test.

KCSAN samples accesses and does not prove that no race exists.

## Mutation testing: `mutation-20261110-045209.log`

`scripts/mutate.py` removes one safety check at a time, rebuilds the driver and
runs the test suite, expecting a failure. All but one of the mutants remove checks
whose removal cannot touch kernel memory. The exception, `unpublish_va`, restores
the second bug. Its test fails on the first stale lookup, in a VM on a throwaway
snapshot.

| Result | Count | Detail |
|---|---|---|
| Killed | 19 of 22 | The suite failed for the intended reason |
| Survived | 3 of 22 | `fill_zero_len`, `fill_max_len`, `copy_zero_len` |

Each survivor is redundant: the range check that follows rejects the same input
with the same error, so the mutant is behaviorally equivalent at the ioctl
boundary. An earlier run had a further survivor, `destroy_pad`. That was a real
test gap, and the test was added.

## Fuzzing

Both runs used 4 VMs with 2 vCPUs each, on the KASAN, lockdep and KCOV kernel under
TCG, with coverage focused on functions named `sgpu_*` and `mychar_*`. The after-fix
run resumed the earlier corpus.

| | Before the fix, `fuzz-20261110-034313` | After the fix, `fuzz-20261110-050107` |
|---|---|---|
| Statistics window | about 50 minutes | about 50 minutes |
| Programs executed | 16,765 | 30,170 |
| Final corpus | 71 | 66 |
| Coverage counter | 2,149 | 2,119 |
| Crashes | 3 VMs hit the same bug, the first after about 12 minutes | 0 |

Before-fix data is in `evidence/bug-002-fuzz-refcount-uaf/`.

The after-fix run shows only that this bug did not recur in that much execution.
syzkaller did not produce a reproducer (`repro=false`), so the deterministic
regression test `test_destroy_unpublishes_va` is what covers the stale lookup.

Coverage from `fuzz-after-fix-filecover.csv`:

| File | PCs covered |
|---|---|
| `driver/sgpu.c` | 216 of 423 (51%) |
| `mychar.c` | 57 of 70 (81%) |

KCOV records only syscall context, so the interrupt thread, watchdog and probe
paths count as uncovered. The stack traces of the second bug show those paths did
run. The functional tests exercise them too, but this number does not measure that.

## Benchmark: `bench-20261110-061259.log`

Debug kernel under TCG, so useful only for comparing the driver with itself.

| Metric | Value |
|---|---|
| Submit-to-fence latency, 2,000 rounds | p50 857 µs, p90 1064 µs, p99 1501 µs, max 7387 µs |
| Throughput, 1 command per submit | 1,235 commands/s |
| Throughput, 8 commands per submit | 9,451 commands/s |
| Throughput, 64 commands per submit | 62,362 commands/s |

Batching gives roughly 50× the command rate of single submits.

## Static checks

`checkpatch.pl --strict`: `driver/sgpu.c` has 0 errors, 0 warnings and 4 style
checks. `mychar.c` has 0 errors, 0 warnings and 1 style check. Sparse is not
installed in this environment and was not run.

## Invalid runs, not counted

| Run | Why it is invalid |
|---|---|
| `vmtest-20261110-060252` | Printed `PASS` although lockdep was off. See below |
| KCSAN `20261110-013601` | The module failed to load, so "0 reports" meant nothing |
| Fuzzing `20261110-021101` | A `/sys` glob check hit the no-output timeout under TCG. 50 executions, no coverage |
| Fuzzing `20261110-031526` | Config error: `focus_areas` at the wrong JSON level, so the manager exited |
| Fuzzing `20261110-031627` | Executors stuck in network device setup. Those features are now disabled in `scripts/fuzz.sh` |

### The run that passed but should not have

`vmtest-20261110-060252` printed `RESULT: PASS`, but the same log showed
`debug_locks: 0` and taint 4128. KASAN had fired 1.7 s into boot, before any module
loaded, in `handle_edge_irq`. The report switched lockdep off for the entire run,
and the script's `dmesg -C` had removed the report before the final scan. The run
also did about 45% more stress work than the valid one, as expected with lockdep off.

`scripts/vm_test.sh` now stops with `RESULT: INVALID` if there is a report during
boot, and also if lockdep is off or the taint is unexpected at the end. The excerpt
is `invalid-run-boot-kasan-excerpt.txt`.

The report appeared in 1 of the 8 saved boot logs from the KASAN kernel. A likely cause,
from reading the source and not confirmed by experiment: `identify_cpu()` briefly
zeroes the CPU feature flags, including LA57. An interrupt in that window sees
`pgtable_l5_enabled()` false, so KASAN's address range check goes wrong and it
reports a valid direct-map address as wild. Neither driver is on the stack.

## Not tested

- Real hardware, or any device other than the QEMU model.
- Runs with an IOMMU, or a malicious device.
- Long soak runs. The longest fuzzing run was one hour.
- The ALSA issue in `alsa-fix-analysis.md`. That document is source analysis only.
