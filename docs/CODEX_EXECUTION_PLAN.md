# AzamiOS Master Execution Plan for Codex / Autonomous LLM Agents

> **Audience:** OpenAI Codex, autonomous AI agents, and kernel developers continuing the AzamiOS project.
> **Scope:** Full Linux userspace ABI compatibility, high performance, enterprise security, and multi-hardware support for x86_64.
> **Standard:** Complete implementations only — no fake functions, no placeholders, zero regressions across boot and ABI suites.

> **Follow-up roadmap (2026-10-05):** [IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md)
> refines the prospective W4–W6 work below against the current source and adds
> Linux semantic, application, and desktop validation milestones. Use its task
> order, prerequisites, skill mapping, and acceptance criteria for new work.
> Sections below retain the earlier roadmap and execution history.

---

## 1. Project Baseline & Current Status

### 1.1 Previously Recorded Milestones
- **Workstream W0 (Test & Boot Harness):**
  - Boot harness: `tests/boot/run.sh` (headless QEMU serial monitor; ABI images require a completed probe, and fatal diagnostics, premature VM exit, or timeout fail the run).
  - Boot matrix: `tests/boot/matrix.sh` testing 10 distinct configurations across KVM (`-cpu host`, SMP 1/4, RAM 512M/1536M, BIOS/UEFI) and TCG (`-cpu qemu64`, `-cpu max`, SMP 1/4, RAM 512M/1536M, BIOS/UEFI).
  - Linux binary harness: `scripts/linux-test.sh` booting ephemeral ext2 initrd running stock musl-compiled `azami-abi-probe`.
- **Workstream W1 (Boot Reliability):**
  - Fixed TCG single-core MWAIT hang in `arch/x86_64/cpu/switch_to.asm`.
  - Fixed `clock_nanosleep` flags validation in `kernel/syscall/syscall.c`.
  - Boot matrix: **10 / 10 passed (0 failed)**.
- **Workstream W2 (Modular Syscall Architecture):**
  - Declarative table: `kernel/syscall/syscalls.tbl` (385 defined syscall numbers, types, entry points).
  - Automated code generation: `scripts/gensyscalltbl.py` generating `kernel/syscall/syscall_table.h`.
  - Prototype and consistency verifier: `scripts/check_syscall_tbl.py` enforcing zero prototype mismatches.
  - Decomposed monolithic 14,281-line `syscall.c` into 9 isolated domain modules:
    - `kernel/syscall/syscall_internal.h`: Shared internal includes and prototypes.
    - `kernel/syscall/sys_fs.c`: Filesystem, I/O multiplexing (`epoll`, `select`, `poll`), `inotify`, `fanotify`.
    - `kernel/syscall/sys_mm.c`: Virtual memory management (`mmap`, `mprotect`, `munmap`, `brk`, `madvise`, `pkey_*`).
    - `kernel/syscall/sys_proc.c`: Process lifecycle (`clone`, `fork`, `execve`, `sched_*`, `prctl`, `ptrace`, `unshare`).
    - `kernel/syscall/sys_signal.c`: Signal delivery, masking, stacks (`rt_sigaction`, `rt_sigprocmask`, `sigaltstack`).
    - `kernel/syscall/sys_net.c`: BSD socket layer, TCP/UDP/Unix domain networking.
    - `kernel/syscall/sys_time.c`: POSIX clocks, timers, monotonic high-res timekeeping.
    - `kernel/syscall/sys_ipc.c`: Futex synchronization, System V IPC, POSIX message queues.
    - `kernel/syscall/sys_misc.c`: `uname`, `sysinfo`, `getrandom`, `kexec`, `bpf`.
    - `kernel/syscall/syscall.c`: Syscall dispatcher, bounds check, path & file descriptor resolution helpers.
  - System V AMD64 ABI register preservation verified: callee-saved registers (`%rbx`, `%rbp`, `%r12-%r15`) and 6-argument raw syscall convention tested with inline assembly.
- **Workstream W3 (Linux ABI Compatibility - Modern Syscalls & Containers):**
  - Syscall status audit: `docs/SYSCALL-STATUS.md` documenting all 385 syscalls (356 implemented = 92.5%, 29 honest ENOSYS stubs, 0 missing).
  - Modern syscall implementations:
    - `renameat2(2)` with `RENAME_NOREPLACE` collision detection, invalid flags rejection (`fs/vfs.c`, `kernel/syscall/sys_fs.c`).
    - `openat2(2)` supporting `RESOLVE_BENEATH` (preventing absolute path or `..` directory traversal escapes) and `RESOLVE_NO_SYMLINKS` (detecting any intermediate or trailing symlink traversal and returning `-ELOOP`) in `kernel/syscall/sys_fs.c`.
    - `epoll_pwait2(2)` supporting nanosecond `timespec` validation (`tv_nsec < 0 || >= 1e9` -> `-EINVAL`), atomic `sigmask` application, and zero-timeout polling in `kernel/syscall/sys_fs.c`.
    - `pivot_root(2)` root swap for calling process container isolation in `kernel/syscall/sys_fs.c`.
    - Container namespaces: implemented `uts_namespace_t` with refcounting (`kernel/sched/sched.h`, `sched.c`), per-process isolation in `sys_unshare_impl` (`CLONE_NEWUTS`, `CLONE_NEWPID`, `CLONE_NEWNS`), per-namespace `uname`, `sethostname`, `setdomainname` (`kernel/syscall/sys_misc.c`).
    - Procfs namespace fidelity: `/proc/[pid]/ns` virtual directory enumerating `uts`, `pid`, `mnt`, `net`, `ipc`, `user`, `cgroup` symlinks resolving to standard Linux `ns:[inode]` format (`fs/procfs.c`).
    - Resolved symlink restart bug in `fs/vfs.c` (`vfs_path_lookup_internal`) preserving `follow_final` state across intermediate symlink hops.
  - Linux ABI conformance probe (`tools/linux/tests/abi-probe.c`): **121 / 121 tests passed (0 failed)**.

---

## 2. Development and Validation Conventions

When modifying this repository, you MUST adhere strictly to these rules:
1. **Professional OS Developer Standard:** Write complete, idiomatic, clean C and assembly code. Never use placeholder code, stubs returning fake success, or comments saying `/* TODO: implement later */`.
2. **Preserve User Data:** Never delete or corrupt `hdd.img` or any user disks. The regression test suite builds ephemeral initrd images in `build/` and leaves disk data intact.
3. **Double Test Gate Enforcement:**
   Every milestone commit MUST be gated by:
   ```bash
   # Gate 1: Linux ABI conformance probe (must report 0 failed)
   ./scripts/linux-test.sh

   # Gate 2: Boot reliability matrix (all available cases must pass;
   # report four unavailable KVM cases separately on TCG-only hosts)
   ./tests/boot/matrix.sh
   ```
4. **Compile Cleanliness:**
   Build the kernel with `make -j$(nproc) build/kernel.elf` and build changed
   native targets with zero new project-code warnings. The default `make`
   target also rebuilds `hdd.img`, so do not use it as a routine regression
   prerequisite. Record upstream compiler warnings separately.

---

## 3. Workstream W3: Userspace Smoke Verification

### 3.1 Task W3.7: BusyBox Applet Smoke Matrix & Dynamic Linking — Implemented
- **Goal:** Verify standard Linux userspace binaries execute out-of-the-box in headless QEMU.
- **Components to Test:**
  1. Stock BusyBox applets: `sh`, `ls`, `ps`, `cat`, `mkdir`, `rm`, `free`, `top`, `uname`, `env`.
  2. Dynamic ELF loader (`userland/ldso/ldso.c`):
     - Ensure `PT_INTERP` correctly maps `/lib/ld-azami.so` or `/lib/ld-musl-x86_64.so.1`.
     - Verify auxiliary vector entries (`AT_RANDOM`, `AT_SECURE`, `AT_SYSINFO_EHDR`, `AT_PHDR`, `AT_PAGESZ`).
     - Test dynamic symbol resolution (`dlsym`, `dlopen`).
- **How to verify:**
  Add a script `tests/smoke/busybox_smoke.sh` that boots headless QEMU with BusyBox init, runs `sh -c "ls -la / && free && ps"`, and asserts successful exit code.

**Execution record (2026-10-05):**
- Added `tests/smoke/busybox_smoke.sh` and its fixtures. A stock static musl
  supervisor runs BusyBox `sh` and checks actual child exit statuses for all
  ten listed applets and both dynamic probes. The final guest result is
  **3 / 3 groups passed, 0 failed**.
- Both `/lib/ld-azami.so` and `/lib/ld-musl-x86_64.so.1` pass argument,
  environment, auxv, `DT_NEEDED`, relocation, constructor, startup TLS,
  `dlopen`, `dlsym`, `dlclose`, and missing-library/symbol error checks.
- Added bounded GNU hash symbol-count support to the native loader and libc
  loader bridge. The fixtures cover both SysV and GNU hash tables.
- Replaced native libc's hard-coded `getauxval` answers with the saved process
  auxiliary vector, including `ENOENT` for absent entries.
- Added an isolated upstream shared musl build (`make -C tools/linux shared-musl`)
  without installing anything into the host's `/lib`. The existing Linux
  `install` target stages the interpreter for regular images when it is built,
  preserving the native `/lib/libc.so`.
- Hardened `scripts/linux-test.sh` against missing/failed verdicts and allocator
  diagnostics. Negative runs confirmed timeout/missing-verdict and nonzero
  child exit failures cannot produce a passing host result.
- Made the regression ISO bootable with BIOS and UEFI. The UEFI matrix now
  waits for the completed ABI probe. This exposed and fixed allocation of
  physical page zero from UEFI usable memory: page zero is now reserved because
  address zero is the PMM/VMM allocation-failure sentinel.
- Regression gates: **121 / 121 ABI checks passed**; **6 / 6 available TCG boot
  configurations passed**, including UEFI. **Four KVM cases were skipped**
  because `/dev/kvm` is unavailable; the earlier 10/10 baseline is not a result
  from this execution. The changed kernel/native code built without warnings.
- All VM disks are disposable images under `build/`; `hdd.img` was not rebuilt
  or attached. The kernel was built with `make -j$(nproc) build/kernel.elf`,
  avoiding the default target's persistent-disk rebuild.

See `tests/smoke/README.md` for reproduction. The next implementation milestone
is **W4.1, the performance micro-benchmark suite**. KVM coverage remains to be
rerun on a host with accessible `/dev/kvm`.

---

## 4. Workstream W4: Performance & Hardware Counters

**Relevant Skills:** `hardware-counters`, `linux-perf`, `flamegraphs`, `cpu-cache-opt`, `virtual-memory-paging-and-tlb`, `kernel-concurrency`, `custom-allocators`.

### 4.1 Micro-Benchmark Suite (`tests/bench`)
Create standalone static C benchmarks measuring:
1. **Syscall Latency (`bench_syscall.c`):** Loop of 10,000,000 raw `getpid()` syscalls; report nanoseconds per syscall.
2. **Context Switch Ping-Pong (`bench_pipe.c`):** Two threads or processes passing a 1-byte token across a pipe; report round-trip context switch time in microseconds.
3. **Memory Fault Throughput (`bench_mmap.c`):** Anonymous `mmap` of 256 MiB followed by sequential and random page writes to trigger kernel demand paging; report MiB/s.
4. **Process Spawning (`bench_fork.c`):** 1,000 iterations of `fork()` + `waitpid()`; report forks/second.

### 4.2 Hardware PMU Exposure

**Continuation — 2026-10-07:** the [thread counter checkpoint](W4.2_COUNTER_RESULTS.md)
and [scoped/remote checkpoint](W4.2_REMOTE_RESULTS.md) are implemented. CPU-wide,
CPU-filtered and other-task counting now uses bounded requests, with validated
TID affinity and credential/dumpability checks. The sysctl defaults to 2. The
[PMU/stock-tool checkpoint](W4.2_HARDWARE_RESULTS.md) adds observed KVM wrap,
user/kernel filtering and attached stock Linux 6.12 `perf stat`. Native/vendor
runtime coverage remains additional work. The [W4.3 FD checkpoint](W4.3_FD_RESULTS.md)
now includes measured safe reference release and the sysfs pagination fix. A
[controlled PMM baseline](W4.3_PMM_RESULTS.md) and
[opt-in exhaustion checkpoint](W4.3_PMM_SELFTEST_RESULTS.md) are recorded.
Real failure/mixed-order/restore checks pass on one/four-CPU KVM/TCG guests.
Deferred reclaim, refill/spill, user-copy and COW measurements remain next. Follow
[IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md) for execution order. The original
proposals below are historical.

- Connect `arch/x86_64/cpu/pmu.c` to `sys_perf_event_open_impl` (`kernel/syscall/sys_misc.c`).
- Enable hardware performance counters:
  - `PERF_COUNT_HW_CPU_CYCLES`
  - `PERF_COUNT_HW_INSTRUCTIONS`
  - `PERF_COUNT_HW_CACHE_REFERENCES`
  - `PERF_COUNT_HW_CACHE_MISSES`
  - `PERF_COUNT_HW_BRANCH_INSTRUCTIONS`
  - `PERF_COUNT_HW_BRANCH_MISSES`
- Expose `/proc/sys/kernel/perf_event_paranoid` (default: 1).

### 4.3 Kernel Fast-Path Optimisations
1. **User Memory Access (`arch/x86_64/cpu/uaccess.asm`):**
   - Use `rep movsb` with ERMS (Enhanced REP MOVSB) on modern CPUs for copies > 128 bytes.
2. **Lockless / RCU Fast Path for File Descriptors:**
   - In `kernel/syscall/syscall.c` (`fget`), use lockless atomic reference bumps rather than acquiring `fd_lock` for read-only lookups.
3. **Page Allocator Magazine:**
   - Add a per-CPU 32-page cache in `kernel/mm/pmm.c` to eliminate global lock contention on 4 KiB page allocations.

---

## 5. Workstream W5: Security & Binary Hardening

**Relevant Skills:** `kernel-security`, `binary-hardening`, `sanitizers`, `fuzzing`, `branch-prediction-and-speculation`.

### 5.1 Kernel Address Space Layout Randomization (KASLR)
- In `arch/x86_64/boot/limine.cfg` and the kernel relocation engine:
  - Generate 64-bit entropy from `RDRAND` / `RDSEED` (with TSC fallback).
  - Align kernel base address to 2 MiB boundary within the high canonical region (`0xFFFFFFFF80000000 - 0xFFFFFFFFFF000000`).
  - Apply ELF relocations (`R_X86_64_RELATIVE`) at early boot before jumping to C `kmain`.

### 5.2 Strict W^X Verification
- Write a page table walker in `arch/x86_64/mm/kprotect.c` that walks all 512 PML4 entries at boot and asserts:
  $$\forall \text{ PTE}, \neg (\text{PTE.RW} \land \neg \text{PTE.NX})$$
  (No page may have both Writeable and Executable permissions simultaneously).

### 5.3 Host libFuzzer Harnesses (`tests/fuzz`)
Compile kernel standalone parser components on the host using `clang -fsanitize=fuzzer,address,undefined`:
- `fuzz_ipv4.c`: Ingests raw Ethernet/IPv4 packet frames into `net_ipv4_process`.
- `fuzz_tcp.c`: Ingests TCP segments into `tcp_process_packet`.
- `fuzz_elf.c`: Ingests malformed ELF headers into `elf_load_binary`.
- `fuzz_path.c`: Ingests malformed path strings into `vfs_resolve_path`.

---

## 6. Workstream W6: Multi-Hardware Architecture & Drivers

**Relevant Skills:** `device-drivers`, `platform-device-model`, `bus-drivers-i2c-spi`, `dma-baremetal`, `hypervisor-internals`.

### 6.1 NVMe Driver Modernization (`drivers/block/nvme.c`)
- Implement MSI-X per-CPU interrupt routing.
- Allocate dedicated Submission Queue (SQ) and Completion Queue (CQ) pairs per CPU core to prevent queue lock serialization.
- Support 4K native sector sizes (LBA formats).

### 6.2 Virtio Subsystem Modernization
- `drivers/net/virtio_net.c`: Support multiqueue virtqueue pairs (`VIRTIO_NET_F_MQ`) and checksum offload (`VIRTIO_NET_F_CSUM`).
- `drivers/block/ahci.c`: Support NCQ (Native Command Queuing) with up to 32 active commands in the command issue register (`P#CI`).

### 6.3 ACPI Hardware Discovery
- Parse ACPI MADT (Multiple APIC Description Table) to map all IOAPIC interrupt redirection entries.
- Parse ACPI SRAT/SLIT tables for NUMA node memory affinity.

---

## 7. Useful Diagnostic Commands Reference

```bash
# Kernel build preserving the workspace HDD
make -j$(nproc) build/kernel.elf

# Rebuild only Linux tools and test binaries
make -C tools/linux

# Run Linux ABI probe inside QEMU
./scripts/linux-test.sh

# Run full boot matrix across KVM and TCG
./tests/boot/matrix.sh

# Regenerate syscall table from syscalls.tbl
python3 scripts/gensyscalltbl.py

# Verify syscall table consistency
python3 scripts/check_syscall_tbl.py

# Update syscall implementation status document
python3 scripts/generate_syscall_status.py
```
