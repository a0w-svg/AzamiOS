# AzamiOS improvement plan — 2026-10-05

This is the follow-up roadmap after W3.7. It refines the future workstreams in
[CODEX_EXECUTION_PLAN.md](CODEX_EXECUTION_PLAN.md) using the current source and
the available development skills. It describes proposed work; creating this
document does not implement or validate these milestones.

**Execution status — 2026-10-06:** W4.1 is implemented. Four static musl
benchmarks passed on Linux and AzamiOS with ten measured repetitions after
warm-up; baseline manifests, samples, statistics, and regression evidence are
saved in [W4.1_BENCHMARK_RESULTS.md](W4.1_BENCHMARK_RESULTS.md). The required
ABI, smoke, and six TCG boot gates passed in the initial sandbox run.
Host KVM access is now verified (API 12); the updated kernel passes all four
KVM and six TCG boot gates. Prior KVM skips reflected sandbox device visibility.
W3.8’s namespace checkpoint now enforces UTS permissions,
protects namespace lifetime and names, exposes real UTS identities, and rejects
unsupported isolation. A futex wake/requeue race found during regression work
is also fixed. See [W3.8_NAMESPACE_RESULTS.md](W3.8_NAMESPACE_RESULTS.md).
The `openat2` checkpoint also implements confinement during component traversal,
counted dirfd lifetime, namespace synchronization, and Linux differential/race
tests. See [W3.8_OPENAT2_RESULTS.md](W3.8_OPENAT2_RESULTS.md). Exact interpreter
loading and failed-exec preservation are now complete: 70 checks pass on KVM
and TCG, and 56 shared observations match Linux. See
[W3.8_EXEC_RESULTS.md](W3.8_EXEC_RESULTS.md). The epoll wait and registration
identity checkpoints now fix timeout overflow, fractional waits, interruption,
signal masks, descriptor lifetime during scans, (file, fd) identity, non-pollable EPERM,
cycle ELOOP, EPOLLET edge triggering, and EPOLLONESHOT: all 109 observations
match Linux on KVM and TCG. See [W3.8_EPOLL_RESULTS.md](W3.8_EPOLL_RESULTS.md).
Additional flag/rename modes are complete (58/58 matching observations, see
[W3.8_RENAME_RESULTS.md](W3.8_RENAME_RESULTS.md)). Per-thread signals (68/68 matching
observations, see [W3.8_SIGNALS_RESULTS.md](W3.8_SIGNALS_RESULTS.md)) and native runtime-loader
concurrency (53/53 passed, see [W3.8_LOADER_RESULTS.md](W3.8_LOADER_RESULTS.md)) are complete.
The recorded W3.8 checkpoints are complete; broader Linux isolation and the
limits described in their execution records remain.

**W4.2 checkpoints — 2026-10-07:** thread and scoped/remote accounting now have
[PMU wrap/filter and stock perf validation](W4.2_HARDWARE_RESULTS.md). Attached,
non-inheriting Linux 6.12 `perf stat` works with software and supported generic/raw
hardware events. KVM/TCG evidence covers 195/136 counter checks, observed 48-bit
wrap and user/kernel filters. Intel mapping is unit-tested; native/vendor runtime
coverage remains additional work. W4.3 now has a
[measured FD reference checkpoint](W4.3_FD_RESULTS.md); continue with PMM,
user-copy and COW measurement. A [controlled allocator baseline](W4.3_PMM_RESULTS.md)
now passes on KVM/TCG with 55 worker records and 66 parked snapshots; allocation
failure and mixed-order evidence is now recorded in the
[opt-in boot checkpoint](W4.3_PMM_SELFTEST_RESULTS.md). Deferred reclaim and
refill/spill measurements remain before cache tuning. Inheritance/exec
workflow, groups and sampling remain later counter milestones. Gemini review
confirmed a sysfs pagination
defect; its fix passes 32/64-CPU enumeration.

## Baseline and corrections

The latest recorded run passed 121 ABI checks, three BusyBox/dynamic-loader
test groups, and all ten boot configurations including UEFI: four KVM and six
TCG. Earlier KVM skips reflected sandbox device visibility.
Preserve the existing workspace changes and
record the exact source state for every subsequent result.

| Area | Current evidence | Next improvement |
| --- | --- | --- |
| Performance counters | `kernel/perf/perf.c` already calls `arch/x86_64/cpu/pmu.c`. Task/CPU-wide counting uses monotonic context time, CPU filters, enforced credentials and bounded remote read/control. Groups and sampling return errors. | Induced KVM wrap, filters and stock attached `perf stat` are validated; additional native/vendor runtime coverage remains. |
| Page allocator | `kernel/mm/pmm.c` already has per-CPU caches with capacity 128 and batch size 64. | Measure contention, memory retention, and allocation failure behavior. |
| User copies | `arch/x86_64/lib/uaccess.asm` already uses `rep movsb`. | Measure copy sizes and verify fault recovery before choosing another copy strategy. |
| NVMe | `drivers/block/nvme.c` uses one synchronous I/O queue pair and controller serialization; namespace discovery accepts 512–4096-byte LBAs. | Validate 4K sectors across the block/filesystem stack; add request tracking, recovery, interrupts, then multiple queues. |
| AHCI | `drivers/block/ahci.c` already has NCQ, with up to seven queued data slots. | Audit completion/reset ownership, then expand queue depth within controller limits. |
| ACPI | `drivers/acpi/ioapic.c` already discovers IOAPICs and interrupt overrides from MADT. | Validate table bounds and routing; add SRAT/SLIT topology before NUMA placement. |
| Kernel protection | `arch/x86_64/mm/kprotect.c` seals kernel sections and HHDM aliases. | Audit effective permissions and aliases, including huge mappings; evaluate relocatable boot separately. |
| Linux isolation | W3.8 namespace tests validate UTS sharing/isolation, permissions, lifetime, and errors; unsupported namespaces return errors. | Implement namespace descriptors/joining and per-thread namespace storage before full PID/mount isolation. |
| Desktop | `USERSPACE-UI-AUDIT.md` records host tests and native builds, but no completed live GUI/audio verification. | Add disposable desktop boot and interaction tests. ABI boot success does not prove desktop success. |

The syscall status table describes implementation presence. It is not evidence
of complete Linux semantics or container compatibility.

## Skills and how to apply them

Read the relevant skill before starting its milestone. Apply its design and
diagnostic principles to AzamiOS's actual APIs; Linux helper names and MCU
register examples are not drop-in implementations for this kernel.

| Skill | Application |
| --- | --- |
| [hardware-counters](../.agents/skills/hardware-counters/SKILL.md) | Event availability, counter widths, CPU-specific event mappings, and measured results. |
| [linux-perf](../.agents/skills/linux-perf/SKILL.md) | Counting conformance and workload profiling. Guest `perf record` remains unavailable until sampling is implemented. Host profiling of QEMU measures the emulator. |
| [kernel-concurrency](../.agents/skills/kernel-concurrency/SKILL.md) | Descriptor lifetime, IRQ/process synchronization, request ownership, and lock ordering. |
| [custom-allocators](../.agents/skills/custom-allocators/SKILL.md) | Existing page-cache tuning, fragmentation, bounded retention, and allocator stress. |
| [virtual-memory-paging-and-tlb](../.agents/skills/virtual-memory-paging-and-tlb/SKILL.md) | Demand faults, COW, protection changes, huge mappings, and TLB invalidation. |
| [device-drivers](../.agents/skills/device-drivers/SKILL.md) | PCI resources, interrupt lifecycle, DMA mappings, teardown, and recovery. |
| [dma-baremetal](../.agents/skills/dma-baremetal/SKILL.md) | Buffer ownership, completion ordering, and coherency principles; use the x86 DMA API rather than STM32 examples. |
| [dynamic-linking](../.agents/skills/dynamic-linking/SKILL.md) | Interpreter identity, symbol lookup, dependency lifetime, constructors, and runtime loading. |
| [abi-and-calling-conventions](../.agents/skills/abi-and-calling-conventions/SKILL.md) | Raw syscall benchmarks, process entry, signal frames, and register preservation. |
| [kernel-security](../.agents/skills/kernel-security/SKILL.md) | Isolation boundaries, counter permissions, effective W^X, and kernel relocation design. |
| [fuzzing](../.agents/skills/fuzzing/SKILL.md) | Real parser coverage, bounded inputs, resettable state, corpora, and reproducible failures. |
| [qemu-for-kernel-development](../.agents/skills/qemu-for-kernel-development/SKILL.md) | Disposable firmware/device matrices, fault diagnosis, and runtime verification. |

## Execution order

Each row is a separately reviewable milestone. Its implementation must include
the relevant regression coverage and failure paths before the next dependent
milestone starts.

| Order | Milestone | Depends on | Completion evidence |
| --- | --- | --- | --- |
| 1 | W4.1: Reproducible benchmarks — complete on Linux/TCG | W3.7 | [Four benchmarks, supervisor, tests, and saved baseline](W4.1_BENCHMARK_RESULTS.md); KVM skipped. |
| 2 | W3.8: Path, exec, and isolation fidelity — complete | Current ABI/smoke harnesses | [Namespaces](W3.8_NAMESPACE_RESULTS.md), [openat2](W3.8_OPENAT2_RESULTS.md), [exec](W3.8_EXEC_RESULTS.md), [epoll](W3.8_EPOLL_RESULTS.md), [rename](W3.8_RENAME_RESULTS.md), [signals](W3.8_SIGNALS_RESULTS.md), [loader](W3.8_LOADER_RESULTS.md). |
| 3 | W4.2: Accurate counting checkpoints complete on AMD KVM/TCG | W4.1 | [Wrap, filters and stock attached perf](W4.2_HARDWARE_RESULTS.md); native/vendor runtime coverage remains additional work. |
| 4 | W4.3: FD checkpoint complete; allocator/copy/COW measurement next | W4.1, W3.8; W4.2 when PMU is needed | Before/after measurements and SMP lifetime/fault stress. |
| 5 | W6.0: Driver test infrastructure | Existing boot harness | Disposable disks, request tracing, failure injection, DMA/IRQ tests. |
| 6 | W6.1: NVMe concurrency and recovery | W6.0 | Multiple outstanding requests, safe reset, MSI-X, multiple queues. |
| 7 | W6.2: Virtio-net offload and queues | W6.0 | Feature negotiation, packet verification, SMP throughput and recovery tests. |
| 8 | W6.3: AHCI completion and NCQ depth | W6.0 | Error recovery, ownership proof, negotiated slot depth, data integrity. |
| 9 | W5.1: Parser fuzzing and protection audit | W3.8; repeat after driver changes | Sanitizer-clean corpus runs and effective mapping audits. |
| 10 | W7.1: Real Unix/Linux workloads | W3.8, W4.2 where needed | Application output, exit status, persistence, and fallback checks. |
| 11 | W7.2: Desktop and audio runtime | Disposable desktop image | Verified actions, saved preferences, input, rendering, and PCM capture. |
| 12 | W6.4 / W5.2: NUMA and relocatable kernel boot | Stable allocator/driver/protection results | Topology/placement tests and independently verified kernel relocation. |

W3.8 security defects take precedence over performance changes. Parser fuzzing
can start early on an isolated parser once its production interface is ready.
This order does not require parallel agents.

## W4.1 — Benchmarks and baseline

**Skills:** hardware-counters, linux-perf, ABI, virtual memory, QEMU.

Add `tests/bench/Makefile`, `bench_syscall.c`, `bench_pipe.c`, `bench_mmap.c`,
`bench_fork.c`, a guest supervisor, and `tests/bench/run.sh`. Build static musl
binaries that can also run unchanged on Linux.

- Syscalls: issue 10,000,000 raw `getpid` calls by default; verify results and
  report ns/call. Include warm-up, configurable counts, and a consumed result
  so compiler optimization cannot remove the work.
- Pipes: fork two processes using separate request/reply pipes. Transfer one
  byte per round trip with checked short-I/O/EINTR handling, EOF handling, and
  child exit verification. Report round-trip latency; it includes pipe work
  and is not a direct measurement of one context switch.
- Memory: use fresh anonymous mappings for sequential and deterministic
  shuffled page orders. Default to 256 MiB with a configurable smaller size.
  Verify contents and report pages faulted/second and logical MiB faulted/second;
  writing one byte per page does not measure streaming memory bandwidth.

**Measured correction (W4.1):** the current AzamiOS anonymous `mmap` eagerly
allocates its pages. Record mapping and first-touch durations separately and
use their sum for logical page/MiB provisioning rates; do not report an actual
demand-fault count or omit eager allocation from the measured cost. Linux and
AzamiOS use the same unmodified benchmark binary.
- Processes: run 1,000 `fork`/`waitpid` iterations by default; verify every
  child status and report forks/second without leaking children or descriptors.

Use checked monotonic timestamps. Record timer resolution, source revision plus
dirty-tree fingerprint, compiler flags, payload hashes, CPU model, accelerator,
firmware, SMP count, RAM, iteration counts, and results in machine-readable form.
Keep separate baselines for TCG, KVM, and physical hardware. TCG validates
execution and catches gross changes; it does not establish real CPU speedups.

**Acceptance:** all four workloads finish correctly on AzamiOS and Linux, and
the runner fails for a failed child, missing result, invalid duration, or
timeout. Collect at least ten measured repetitions after warm-up and report
median and spread. Adopt an optimization only when its improvement exceeds the
observed noise and other workloads show no unexplained regression.

## W3.8 — Linux semantic fidelity

**Skills:** ABI, dynamic-linking, kernel-concurrency, kernel-security, QEMU.

**Status — 2026-10-06:** namespace, `openat2`, interpreter/exec, epoll wait and registration identity, rename, per-thread signals, and native runtime loader concurrency checkpoints complete; milestone W3.8 is fully complete. [The execution record](W3.8_NAMESPACE_RESULTS.md) contains the
Linux comparison, SMP stress, regression evidence, and supported API limits.
The [path execution record](W3.8_OPENAT2_RESULTS.md) includes Linux comparisons,
SMP races, and all ten boot gates. The [exec execution record](W3.8_EXEC_RESULTS.md)
contains exact interpreter loading, failure preservation, 70 guest checks, 56
Linux matches, and the same regression gates. The [epoll wait and registration record](W3.8_EPOLL_RESULTS.md)
contains 109 shared Linux observations on KVM and TCG, signal-mask handling,
overflow deadlines, concurrent descriptor operations, (file, fd) keys, EPERM/ELOOP
checks, EPOLLET edge-triggering, and EPOLLONESHOT. The [rename record](W3.8_RENAME_RESULTS.md)
contains 58 shared Linux observations with zero guest skips, `renameat2`, `RENAME_NOREPLACE`,
`RENAME_EXCHANGE`, trailing slash resolution, dot/dotdot handling, and concurrent stress.
The [per-thread signals record](W3.8_SIGNALS_RESULTS.md) contains 68 shared Linux
observations with zero guest skips, independent thread masks (`pthread_sigmask`), directed signaling
(`tkill`/`tgkill`/`rt_tgsigqueueinfo`), thread vs process pending queues, per-thread `sigaltstack` with
onstack `-EPERM` protection, `sigsuspend`, and `rt_sigtimedwait`.
The [dynamic loader record](W3.8_LOADER_RESULTS.md) contains 53 automated checks
covering recursive futex mutex locking, constructor reentrancy, reverse-order destructor sequencing
(`DT_FINI_ARRAY` and reverse dependencies), transitive symbol scope (`dlsym`), `dladdr()`,
thread-local `dlerror` isolation across threads, transactional rollback on failed loads, and
concurrent multi-threaded load/unload stress.

Work in `kernel/syscall/sys_fs.c`, `fs/vfs.c`, `kernel/sched/elf.c`,
`kernel/syscall/sys_proc.c`, and the native loader/libc. Add focused suites under
`tools/linux/tests` and `tests/smoke` rather than expanding one probe indefinitely.

1. **Checkpoint complete:** make `openat2` resolution constraints part of the
   VFS traversal and remove checks before a separate open. Test symlinks
   escaping a dirfd root and concurrent rename/symlink replacement. Hold a
   counted dirfd reference; lexical prefix checks do not establish confinement.
2. Compare flag, mode, structure-size, pointer, and errno behavior with Linux.
   Cover invalid and forward-compatible `open_how` inputs, rename collisions,
   symlink loops, and `epoll_pwait2` interruption and timeout overflow.
   **Wait checkpoint complete:** nanosecond deadlines, overflow saturation,
   temporary-mask lifetime, `EINTR`, and pinned scans now match the focused
   Linux comparison.
   **Rename checkpoint complete:** `renameat2`, `RENAME_NOREPLACE`, `RENAME_EXCHANGE`,
   trailing-slash semantics, POSIX special name (`.`/`..`) `EBUSY` handling, type mismatch
   errors, and atomic concurrent exchange now match Linux (58/58 common cases).
   **Epoll registration identity and modes checkpoint complete:** `(file, fd)` key identity,
   open file description lifetime, non-pollable `EPERM`, cycle detection `ELOOP`, `EPOLLET`
   edge triggering, and `EPOLLONESHOT` now match Linux (109/109 common cases).
   **Per-thread signals checkpoint complete:** independent thread masks (`pthread_sigmask`),
   directed delivery (`tkill`/`tgkill`/`rt_tgsigqueueinfo`), thread vs process pending queues,
   per-thread `sigaltstack` with active onstack protection, `sigsuspend`, and `rt_sigtimedwait`
   now match Linux (68/68 common cases).
3. **Checkpoint complete:** load the exact `PT_INTERP` path and propagate interpreter open/validation/map
   failures. Test missing, malformed, truncated, oversized, and incompatible
   interpreters. Do not silently substitute another libc's interpreter.
   Audit failed exec cleanup and preservation of the caller's existing image.
4. Test namespaces by changing a resource in one process and observing another.
   UTS names, PID translation/reaping, mounts, and procfs namespace descriptors
   must reflect real state. Correct unsupported flag handling before advertising
   container isolation; split full PID and mount namespaces into later milestones.
5. **Checkpoint complete:** extend native runtime-loading tests for concurrent load/unload, repeated
   dependency loads, constructor reentrancy, destructor order, `dlerror` state,
   symbol scope, and failed-load cleanup. Design locking around constructors,
   which can call back into the loader. Keep unsupported runtime TLS explicit
   until module storage and thread lifecycle support are complete. See [W3.8_LOADER_RESULTS.md](W3.8_LOADER_RESULTS.md).

**Acceptance:** deterministic cases match Linux's observable results; race tests
cannot escape the chosen root or access freed objects; unsupported operations
return documented errors. All prior ABI and smoke tests remain green.

## W4.2 — Counter accuracy before sampling

**Status — 2026-10-07:** the thread, scoped/remote and
[PMU/stock-tool checkpoints](W4.2_HARDWARE_RESULTS.md) are implemented. Bounded
CPU requests support CPU-wide/filtered and other-task counting, and real KVM
counter wrap, user/kernel filters and attached stock `perf stat` now pass.
AMD KVM and TCG are validated; Intel mapping has production-code unit tests.
Native/vendor runtime evidence remains additional coverage. W4.3 FD release is measured;
inheritance/enable-on-exec, groups, multiplexing and sampling remain later work.

**Skills:** hardware-counters, linux-perf, kernel-security, kernel-concurrency.

Improve `kernel/perf/perf.c`, `kernel/perf/perf.h`, `arch/x86_64/cpu/pmu.c`,
scheduler hooks, and the procfs/sysctl implementation.

- Test enable/disable/reset/read, FD lifetime, task exit, fork/exec, migration,
  counter wrap, privilege filters, and unsupported attribute combinations.
- Enforce CPU selection or reject unsupported selection explicitly. Implement
  remote counter reads with a bounded cross-CPU mechanism before promising
  per-CPU accounting. Never read a remote CPU's counter locally.
- Replace tick-based durations with a validated high-resolution time source.
  Define live reads and exclude time while an event is not scheduled.
- Expose and enforce `perf_event_paranoid` consistently with credentials and
  capability checks. Test self, other-task, system-wide, and kernel visibility.
- Verify event mappings against detected CPU capabilities. Unsupported AMD
  cache events and unavailable PMUs must produce errors, not synthetic counts.

**Acceptance:** software events have deterministic tests on TCG; supported
hardware counts increase under controlled workloads on PMU-capable KVM/hardware.
Verify an unrelated busy process does not inflate a task-scoped count. Report
unavailable hardware as skipped. Test a pinned stock `perf stat` command only
after checking which ABI features it actually requests.

Sampling, event groups, multiplexing, and `perf record` form a later milestone:
they require overflow handling, bounded ring buffers, lost-record accounting,
and scheduler/permission tests. A counting FD alone does not provide profiling.

## W4.3 — Optimize measured bottlenecks

**FD checkpoint — 2026-10-07:** [results and evidence](W4.3_FD_RESULTS.md)
record safe nonfinal file-reference release, pinned fcntl operations, and the
Gemini-confirmed sysfs pagination fix. Linux/KVM/TCG lifetime checks pass;
KVM four-thread eventfd latency falls 10.2–17.3% against two immutable baseline
batches. Lookup remains FD-locked and final teardown remains namespace-locked.
Explicit-close lock ownership and concurrent status-flag updates remain limited.
The [controlled one/four-worker baseline](W4.3_PMM_RESULTS.md) now records
page integrity and approximate retention on KVM/TCG, with no allocator changes.
The [opt-in exhaustion checkpoint](W4.3_PMM_SELFTEST_RESULTS.md) now covers real
failures, mixed orders and exact restoration on one/four-CPU KVM/TCG guests.
It reproduces one stranded remote cached page and recovery through an owner
drain. Allocation policy is unchanged; production callers holding heap/VMM
locks require an audited deferred-reclaim design. Refill/spill performance
measurement remains before cache tuning.

**Skills:** kernel-concurrency, custom-allocators, virtual memory, hardware-counters.

- Stress `fget`, close, dup, shared FD tables, and exit before changing lookup
  locking. A refcount increment after an unlocked pointer load can touch freed
  memory. Keep the existing lock unless measurements justify a design with
  deferred reclamation and a verified reader protocol.
- Benchmark the existing PMM caches under 1/4-core fault, fork, and mixed-order
  allocation workloads. Measure retained pages, fragmentation, refill/drain
  work, and allocation failures. Add bounded reclaim of idle CPU caches if
  measured starvation requires it; changing the cache size alone is not a fix.
- Test user-copy faults at both ends of a page boundary, partial copies, SMAP
  state restoration, and exception fixups. Benchmark small and large copies
  before selecting ERMS/vector thresholds. Vector use needs a kernel SIMD/FPU
  ownership design.
- Stress COW and concurrent `mprotect`/`munmap` across CPUs. Optimize range
  updates or shootdown batching only after checking remote invalidation and
  page-table/frame reclamation ordering.

**Acceptance:** preserve data and lifetime invariants under SMP stress; publish
the workload and before/after statistics for each accepted optimization.

## W6.0–W6.3 — Hardware throughput and recovery

**Skills:** device-drivers, kernel-concurrency, DMA, QEMU.

Create `tests/hardware` runners using generated scratch disks and isolated
networks. First audit PCI interrupt support, the wait/completion primitives,
DMA masks/mappings in `kernel/mm/dma.c`, and the block API in
`drivers/block/block.h`. Extend interfaces when needed before forcing an
asynchronous driver into a synchronous contract.

For every queue, define request states, who owns each DMA buffer, when it may be
freed, and how a timeout races with completion/reset. Issue and completion paths
must use the required ordering; coherent DMA does not remove publication-order
requirements. Test allocation failures and unwind partially initialized devices.

**NVMe:** replace controller-wide synchronous serialization with command IDs,
request tracking, and bounded completion processing. Add interrupt-driven
completion and MSI-X routing with a tested polling fallback. Negotiate queue
counts and depth before assigning CPU-local queues. Complete/reset/quarantine
outstanding commands before recycling DMA memory. Test PRP boundaries, transfer
limits, flush, out-of-range requests, concurrent I/O, reset, and 512/4096-byte
namespace images. Sector support must include partition and filesystem consumers.

**Virtio-net:** implement validated checksum metadata and completion ownership
before negotiating checksum features. Add multiqueue negotiation, required
control-queue operations, per-queue state and interrupt routing, and fallback to
one pair. Test features on/off, descriptor exhaustion, malformed lengths,
link changes, sustained bidirectional traffic, and packet checksums verified by
an independent peer. Measure drops, IRQ load, throughput, and tail latency.

**AHCI:** audit existing NCQ error paths and slot ownership before raising depth.
Ensure reset stops device access before a slot or bounce buffer is reused; a
stale completion must not complete a new request. Size command/DMA storage for
the negotiated controller and drive limits, up to 32 hardware slots while
respecting control-command requirements. Test NCQ on/off, mixed read/write,
flush ordering, timeout, reset, and integrity under concurrent requests.

**Acceptance:** no mismatched data, duplicate completion, stranded request,
unbounded IRQ work, or DMA-after-free during failure tests. Compare measured
1/4-core throughput against each driver's baseline. Record emulated-device
coverage separately from physical controller validation.

## W5.1 — Parser fuzzing and effective kernel permissions

**Skills:** fuzzing, kernel-security, virtual memory.

Create `tests/fuzz` around actual production parsing code. The current input
functions include `ipv4_input` and `tcp_input`; the old roadmap's
`net_ipv4_process`/`tcp_process_packet` names do not match these sources.
Extract bounded, reusable validation helpers where full kernel entry points
depend on scheduler, VFS, or MMIO services, and call those helpers from both
production and host tests. Do not fuzz a simplified replacement parser.

Start with ELF/program-header and GNU-hash validation, VFS path components,
IPv4 fragmentation/length handling, TCP options, then ACPI record lengths.
Build host targets with libFuzzer, ASan, and UBSan; reset state between inputs,
bound memory/time, retain valid seeds, minimize failures, and turn each bug into
a regression case. Use fixed-run corpus replay plus 60-second fuzz smoke runs;
longer campaigns record duration, input limits, and source fingerprint.

Extend the kernel protection audit to effective permissions through all page
table levels, 4K/2M/1G leaves, and HHDM aliases. Walk valid kernel mappings safely
without following recursive slots indefinitely. Verify kernel text/rodata,
data/stacks, MMIO, and generated-code transitions against their intended policy.
Avoid globally banning userspace RWX as a side effect of kernel W^X auditing;
any stricter userspace policy needs a separate compatibility decision.

**Acceptance:** sanitizer-clean saved corpora, reproducible crash handling, and
an audit that detects deliberately introduced bad mappings in a disposable VM.
Generated code still executes after the intended RW-to-RX transition.

## W7.1–W7.2 — Applications and desktop evidence

**Skills:** dynamic-linking, ABI, QEMU, kernel-concurrency.

Expand coverage for the Linux binaries already built by `tools/linux/ports.mk`:
shell pipelines/redirection/signals, tar/gzip round trips, TinyCC compiling and
running a program, Lua scripts, and SQLite commit/reopen/rollback/WAL workloads.
Validate output and disk state, not merely executable startup. Save syscall
failures and add the smallest reproducer before implementing the missing ABI.

Add glibc tests with a matched, identified loader/libc payload and controlled
library search paths. Pin package sources and record checksums/build settings;
copying whatever happens to exist in the host's library directories is not a
reproducible compatibility baseline. Broader ports such as Git follow after
their filesystem, process, thread, and network requirements pass focused tests.

For the desktop, build a private root tree and disposable ISO; wait for desktop
readiness, then exercise real QEMU input and capture guest output/screenshots.
Cover Settings changes and persistence, Paint clear/undo, Screenshot retake/save,
File Manager actions, terminal launch, clock refresh, resize/scroll, and reported
device failures. Validate PCM capture where supported and report physical audio
output separately. Keep `scripts/test_userspace_ui.sh` as the host gate.

**Acceptance:** workload results survive reopen/restart as appropriate, every
action reports actual success/failure, and the UI audit gains dated runtime
evidence without rewriting its historical observations.

## Later milestones — NUMA and relocatable kernel boot

**NUMA:** validate MADT/IOAPIC bounds and interrupt overrides first, then parse
SRAT/SLIT into a checked topology with a single-node fallback. Introduce node
ownership in PMM, node-local allocation with bounded fallback, and scheduler
placement in separate changes. Publish topology and test allocation/migration
under a multi-node QEMU setup before claiming physical NUMA performance.

**Kernel relocation/KASLR:** start with an ELF/assembly/linker audit and a design
decision on Limine relocation support versus a kernel relocation path. The
current `-fno-pie`/`-mcmodel=kernel` build cannot become relocatable by adding
random arithmetic. Use the real `limine.conf`, `scripts/kernel.ld`, and boot
entry sources. Handle relocations, absolute references, early mappings, SMP
startup, exception tables, and symbol reporting before enabling random placement.
Use kernel entropy when available and document any fallback's limitations.
Verify address changes across boots with BIOS/UEFI and SMP, retain a diagnostic
disable option, and rerun effective-permission and ABI tests.

IOMMU isolation, power management, additional architectures, full container
runtimes, and `io_uring` remain separate designs after these prerequisites.

## Validation and completion rules

For each code milestone, run the existing gates against freshly built artifacts:

```sh
make -j$(nproc) build/kernel.elf
make -C tools/linux probe
python3 scripts/check_syscall_tbl.py
./scripts/linux-test.sh
./tests/smoke/busybox_smoke.sh
./tests/boot/matrix.sh
```

Also run the milestone's meaningful correctness/stress tests; add
`scripts/test_userspace_ui.sh` for desktop changes. Build the changed native
targets and keep project-owned changes warning-free. Record upstream compiler
warnings separately rather than hiding them or silently patching stock packages.

Pass every available required configuration. Report unavailable KVM, PMU, NUMA,
or physical-device coverage as skipped, with the reason; never turn skips into
passes or assert ten boot successes on a six-case host. Release validation must
include the missing configurations on suitable hardware.

Use disposable images under `build/` and preserve `hdd.img` and user data. Avoid
the default disk-building target or `make clean` as a routine test prerequisite.
Store logs, manifests, raw benchmark samples, and failure reproducers. A milestone
is complete when implementation, errors/cleanup, relevant tests, measured claims,
and the execution record are all reviewable. Update status documentation after
behavior changes; never replace unsupported operations with fake success.

The first executable work item, W4.1, now has the four real benchmark sources,
runner, data format, Linux comparison, and saved guest baseline. See its
[execution record](W4.1_BENCHMARK_RESULTS.md). W3.8's namespace checkpoint is
recorded in [W3.8_NAMESPACE_RESULTS.md](W3.8_NAMESPACE_RESULTS.md), and path
confinement in [W3.8_OPENAT2_RESULTS.md](W3.8_OPENAT2_RESULTS.md). Interpreter/exec
fidelity is recorded in [W3.8_EXEC_RESULTS.md](W3.8_EXEC_RESULTS.md), epoll wait
semantics in [W3.8_EPOLL_RESULTS.md](W3.8_EPOLL_RESULTS.md), rename semantics
in [W3.8_RENAME_RESULTS.md](W3.8_RENAME_RESULTS.md), and per-thread signals in
[W3.8_SIGNALS_RESULTS.md](W3.8_SIGNALS_RESULTS.md). The [native loader checkpoint](W3.8_LOADER_RESULTS.md) records concurrency and
runtime-loading coverage. Continue W4.3 controlled allocator, copy and COW
measurements after the completed FD reference checkpoint. The hardware/GUI roadmap below
remains future work.
# AzamiOS Codex Improvement Plan: Hardware, Performance & GUI

This roadmap expands upon the foundational milestones to specifically target **hardware support**, **system performance**, a **fancy graphical look (modern UI)**, and **strict Linux compliance** (ABI and APIs like DRM/KMS, ALSA/evdev).

## 1. Linux-Compliant Hardware Support

To support real Linux binaries (e.g., standard compositors like Weston/Sway, X11, PulseAudio), our drivers must expose standard Linux ABIs.

### 1.1 Graphics (DRM / KMS)
- **Goal:** Expose the Linux Direct Rendering Manager (DRM) and Kernel Mode Setting (KMS) `ioctl` interfaces for `virtio-gpu`.
- **Tasks:**
  - Implement `/dev/dri/card0`.
  - Add standard DRM ioctls: `DRM_IOCTL_MODE_GETRESOURCES`, `DRM_IOCTL_MODE_GETCONNECTOR`, `DRM_IOCTL_MODE_CREATE_DUMB`, `DRM_IOCTL_MODE_MAP_DUMB`, `DRM_IOCTL_MODE_ADDFB`, `DRM_IOCTL_MODE_SETCRTC`, `DRM_IOCTL_MODE_PAGE_FLIP`.
  - Support `dma-buf` for zero-copy sharing between drivers and userspace.

### 1.2 Audio (ALSA / OSS)
- **Goal:** High-fidelity sound output via HDA (Intel High Definition Audio) and AC97 using standard Linux interfaces.
- **Tasks:**
  - Implement `/dev/snd/pcmC0D0p` (ALSA interface) or `/dev/dsp` (OSS compatibility).
  - Support memory-mapped (mmap) audio ring buffers for zero-latency audio playback.
  - Test with a ported standard audio server (e.g., tinyalsa, PipeWire, or PulseAudio).

### 1.3 Input Devices (evdev)
- **Goal:** Standardized input events for keyboards, mice, and touchpads.
- **Tasks:**
  - Implement `/dev/input/eventX`.
  - Expose `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGBIT` ioctls.
  - Generate standard `input_event` structs (EV_KEY, EV_REL, EV_ABS) so `libinput` and Linux X11/Wayland work unmodified.

## 2. Uncompromising Performance

A "fancy look" requires strict 60+ FPS rendering and zero visual stuttering.

### 2.1 Zero-Copy & DMA
- **Goal:** Never copy a frame buffer or an audio buffer on the CPU if it can be avoided.
- **Tasks:**
  - Implement `mmap` for `/dev/dri/card0` dumb buffers.
  - Connect Virtio-GPU to the page allocator to share pages directly with the host hypervisor.

### 2.2 Multi-queue Block I/O (NVMe)
- **Goal:** Prevent UI freezing during heavy disk I/O.
- **Tasks:**
  - Move NVMe and AHCI drivers to a multi-queue block layer.
  - Implement hardware interrupts (MSI-X) rather than polling.
  - Implement `O_DIRECT` and asynchronous I/O (`io_uring` or `AIO`).

### 2.3 Scheduler & Preemption
- **Goal:** Low-latency response for UI and audio threads.
- **Tasks:**
  - Implement a Completely Fair Scheduler (CFS) or similar `SCHED_FIFO` / `SCHED_RR` realtime classes.
  - Ensure the kernel is preemptible to allow high-priority audio threads to run immediately upon hardware interrupts.

## 3. The "Fancy Look" (Modern Compositing UI)

Once the DRM/KMS and `evdev` foundations are Linux-compliant, we can run modern graphical stacks.

### 3.1 Wayland Compositor Port
- **Goal:** Port a lightweight Wayland compositor (e.g., `sway` or `weston`) natively using our DRM/evdev drivers.
- **Tasks:**
  - Compile `wayland`, `libdrm`, `libinput`, and `pixman` via our Linux porting system (`tools/linux/ports.mk`).
  - Render a hardware-accelerated mouse cursor using KMS planes.

### 3.2 High-Fidelity Theming & UI Framework
- **Goal:** A beautiful native application environment.
- **Tasks:**
  - Port `FreeType` for smooth, anti-aliased font rendering.
  - Port `Cairo` or `Skia` for vector-based UI drawing (rounded corners, gradients, drop shadows).
  - Develop or port a lightweight toolkit (e.g., GTK3/GTK4, or a custom AzamiOS toolkit in `userland/`) that uses Wayland as its backend.
  - Implement a dock/taskbar with smooth animations and transparency (using Wayland alpha channels).

## 4. Execution Order (W8.0 - W10.0)

| Phase | Milestone | Description |
| --- | --- | --- |
| **W8.0** | **Input & Display Foundations** | Implement `/dev/input/eventX` and basic `/dev/dri/card0` (DRM dumb buffers). Verify by running a Linux framebuffer test app. |
| **W8.1** | **Virtio-GPU Hardware Acceleration** | Add 2D/3D command submission and page-flipping to the DRM driver. Eliminate screen tearing. |
| **W9.0** | **Audio Subsystem** | Implement ALSA/OSS mmap ring buffers for AC97/HDA. Verify by playing a WAV file via a ported `aplay`. |
| **W9.1** | **System Responsiveness** | Enable Kernel Preemption, MSI-X for NVMe, and `SCHED_RR` for audio/UI threads. |
| **W10.0**| **The Fancy Look (Wayland & UI)** | Port Wayland/Weston, FreeType, and Cairo. Boot directly to a graphical desktop with vector fonts, a taskbar, and hardware cursor. |
