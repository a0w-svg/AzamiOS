# W3.8 Linux semantic tests

Run the namespace comparison with a stock static musl binary:

```sh
tests/semantics/namespaces.sh
tests/semantics/namespaces.sh --output build/semantics/my-namespace-run
python3 -B tests/semantics/test_runner.py
tests/semantics/openat2.sh
tests/semantics/exec.sh
tests/semantics/epoll.sh
```

The wrapper builds only the kernel ELF and probe. The runner uses a new
directory under `build/semantics`, creates disposable boot media through
`scripts/linux-test.sh`, and runs a four-vCPU q35 BIOS guest with 1536 MiB RAM.
KVM is selected if accessible; otherwise it uses TCG. No user disk is attached
or rebuilt. The default host/guest deadline is 120 seconds; `--timeout` changes
it. Timeouts and interrupts terminate the runner's child process group.

On Linux, auto mode first checks `unshare -Ur true`; if available, the probe
runs in a disposable user namespace with the capabilities needed to create a
UTS namespace. The probe creates its own UTS namespace before changing names,
so it does not change the host hostname. `--linux-mode userns` requires this
setup; `--linux-mode native` uses existing capabilities. Unavailable privileged
Linux coverage is explicitly recorded as skipped. AzamiOS skips fail the run.

The cases verify hostname/domain bounds and pointers, empty and embedded-NUL
names, clone/clone3 permissions and isolation, shared fork state, inheritance
through exec, unsupported flag atomicity, live sibling rejection, procfs
identity and lifetime, and atomic name reads during concurrent writes. Guest
policy cases deliberately differ from Linux: namespace types AzamiOS cannot
isolate must return errors. Only shared observations are compared with Linux.

Each run saves `linux.txt`, `guest.txt`, `comparison.json`, and `manifest.json`.
The manifest records the kernel and payload hashes, Git revision, dirty source
inventory and content fingerprint, compiler, commands, machine settings,
timestamps, skips, and result. Full serial logs and disposable media remain
under its `vm/` directory. `NSCASE` records contain names and integer observed
and expected values; `NSSKIP` records contain exclusions. Missing/inconsistent
verdicts, duplicate or failed cases, malformed records, fatal kernel diagnostics,
command failures, and timeouts fail validation.

The separate futex regression exercises 2,000 rapid thread exits/joins,
200 untimed requeues, 200 timed requeue wake/expiry races, and stale queue
cleanup. A timed wait may legitimately expire before a wake under host load;
both terminal outcomes are checked, and some successful wakes are required.
The executable also has a 60-second alarm, while the guest runner supervises
boot and execution independently:

```sh
make -C tools/linux toolchain futex-race-probe
timeout 65s tools/linux/out/bin/azami-futex-race-probe
LINUX_TEST_WORK=build/semantics/futex-vm LINUX_TEST_TIMEOUT=90 \
  scripts/linux-test.sh tools/linux/out/bin/azami-futex-race-probe
```

[The execution record](../../docs/W3.8_NAMESPACE_RESULTS.md) and
saved evidence (local archive: `evidence/2026-10-06/namespaces/`) describe the checkpoint.

The `openat2` wrapper uses the same checked `NSCASE`/`NSSKIP` protocol and
supervisor, selecting `--suite openat2`. It compares Linux and guest results
for confinement, symlink ordering, structure extensions, pointers/flags/modes,
creation, unlink lifetime, renamed directory FDs, and five SMP race groups.
Each race performs 5,000 opens while a concurrent writer replaces symlinks,
moves directories, closes/reuses a dirfd, or changes cwd. Successful confined reads must
contain the inside marker; legitimate transient errors are checked. The FD
reuse test allows either directory's contents because reuse may select either
anchor, while the held reference must survive the selected operation. The
probe removes only its own unique temporary tree, without following symlinks.

Supported resolve modes are `BENEATH`, `IN_ROOT`, `NO_SYMLINKS`, and
`NO_MAGICLINKS`. `NO_XDEV`/`CACHED`, `O_PATH`, `O_TMPFILE`, direct/asynchronous/
synchronous I/O modes, and access mode 3 return `EOPNOTSUPP`. Unknown flags,
conflicting constraints, and invalid modes return `EINVAL`. Positive dentry
cache reclamation and general Linux mount-path identity remain future work;
ambiguous parent chains under scoped resolution fail closed with `EXDEV`.
The VFS's existing 512-byte path buffers remain a limit.

The sandbox may hide `/dev/kvm` even when the host has it. On this host, run
the test wrapper through the approved host execution context for KVM; regular
sandbox runs use TCG. The manifest records the selected accelerator.
See [path results](../../docs/W3.8_OPENAT2_RESULTS.md).

Primary Linux references used for permissions, inheritance, locking, and
namespace lifetime: [UTS implementation](https://github.com/torvalds/linux/blob/master/kernel/utsname.c),
[namespace creation/unshare](https://github.com/torvalds/linux/blob/master/kernel/nsproxy.c),
and [clone flag checks](https://github.com/torvalds/linux/blob/master/kernel/fork.c).

The `exec` wrapper builds a static musl supervisor, a real interpreter entry,
and a stock musl PIE. It selects `--suite exec` in the same checked runner.
The probe creates ELF fixtures in its own temporary tree and compares 56 shared
Linux observations. Guest coverage also checks seven staged error/preservation
pairs, including a late argument-stack overflow after image mapping. Failed
exec must preserve memory regions, mprotect, heap break, TLS, descriptors,
signal handlers and alternate stack, a demand-fault mapping, and live siblings.
Success checks auxv, argument/environment transfer, CLOEXEC, ordinary FD
inheritance, and signal reset. The guest has a valid alternative musl loader;
missing requested interpreters must still fail. The real PIE and shared musl
are copied only into the disposable initrd, and their hashes enter the manifest.

```sh
tests/semantics/exec.sh --output build/semantics/my-exec-run
```

See [exec results and limits](../../docs/W3.8_EXEC_RESULTS.md) and
saved evidence (local archive: `evidence/2026-10-06/exec/`). Executable permission, execveat,
argument/path capacity, thread-directed signals, and concurrent successful
exec remain partial; the results distinguish those limits from tested behavior.

The `epoll` wrapper builds a static musl probe and selects `--suite epoll` in
the same runner. All 84 observations are shared with Linux, with no exclusions.
It checks zero, finite, infinite, sub-millisecond, and overflowing timeouts;
invalid pointers, sizes, and descriptors; level readiness and hangup; and
signal-mask restoration on success, timeout, error, and interruption. Handlers
check temporary masks, action masks, `SA_NODEFER`, and `EINTR` despite
`SA_RESTART`. Signal pending sets, timed signal consumption, and signalfd
readiness check the common Linux signal-bit representation. SMP coverage runs
2,000 scans during concurrent interest changes and 20 waits while a sibling
closes the epoll descriptor. A fatal 40-second alarm bounds the probe.

```sh
tests/semantics/epoll.sh --output build/semantics/my-epoll-run
```

Timeout observations require no early return and a two-second upper bound;
the raw `EPTIME` records are correctness evidence, not a latency benchmark.
The kernel still stores signal masks at process scope. Threaded descriptor
tests explicitly restore the supervisor mask after joins; masked-wait cases
run before them and use child processes as senders. Registration identity,
edge/oneshot modes, nested epoll, ready-list fairness, and full signal/thread
semantics remain incomplete. See [epoll results](../../docs/W3.8_EPOLL_RESULTS.md)
and saved evidence (local archive: `evidence/2026-10-06/epoll/`).

## W4.2 performance-counter checkpoints

```sh
tests/semantics/perf.sh --output build/semantics/my-perf-run
tests/semantics/perf_timeout.sh --output build/semantics/my-perf-timeout-run
tests/semantics/perf_stock.sh --output build/semantics/my-stock-perf-run
tools/tests/pmu_mapping_test.sh
```

The stock-musl probe compares task and CPU-filtered clocks, enabled/running
context time, freeze/resume/reset, ID/read/ioctl layouts, no-ring poll, dup/close,
fork/exec/CLOEXEC, target exit, COW faults, migration, ID uniqueness and 600
concurrent FD lifetimes. CPU-wide software events also exercise real remote
request delivery on TCG. Guest cases cover attribute errors, signed paranoid
policy, distinct credentials, non-dumpable/more-privileged targets, capability
checks and dumpability inheritance/reset. The runner requires these scope cases.

An unavailable PMU is the only permitted guest skip. With a usable PMU the suite
requires CPU-wide/filtered instruction counts, independent caller/worker affinity,
remote read/control, migration and exit races. It also checks stable shared slots
and excludes a busy sibling from sleeping-thread counts. Host kernel/CPU-wide
permission restrictions are recorded as exclusions. Clock checks bracket their
syscalls before serial logging; the results are correctness observations.

The separate timeout runner boots with `smp_rpc_drop_cpu=1`, requires bounded
`ETIMEDOUT` for all four operations, and checks 70 failed-open rollbacks with
continued local counting. Its manifest records fault injection and final hashes.
`tools/tests/smp_rpc_state_test.c` exercises delayed-running ownership, stale
request generations and 100,000 completion/cancel races using production helpers.
The stock-tool runner stages checksum-pinned unmodified Linux 6.12 perf and
requires positive exact-event CSV counts and running time for attached counting.
It enables a boot-only PMC wrap selftest and requires a real width crossing when
instructions are available; TCG explicitly skips hardware. Fixed-loop probe
cases test user/kernel filters and raw/generic instructions, while the host
production mapper test covers both vendor tables and all availability masks.
See [current hardware/tool results](../../docs/W4.2_HARDWARE_RESULTS.md),
[scoped/remote results](../../docs/W4.2_REMOTE_RESULTS.md) and the
[historical thread checkpoint](../../docs/W4.2_COUNTER_RESULTS.md).

FD lifetime and sysfs pagination checkpoints are available as:

```sh
tests/semantics/fd_lifetime.sh --output build/semantics/fd-current
tests/semantics/sysfs_pagination.sh --smp 32 --output build/semantics/sysfs-32
tests/semantics/sysfs_pagination.sh --smp 64 --output build/semantics/sysfs-64
```

The FD suite requires the same 21 checks on Linux and AzamiOS, including failed
duplicate-install cleanup, concurrent close/reuse, observable in-flight read
ownership, and lock preservation across temporary I/O/fcntl references. The
sysfs suite uses small direct getdents64 buffers, checks continuation/cookies,
lookup consistency and exact CPU enumeration; the host runner additionally
requires the actual present and enumerated guest CPU counts to equal `--smp`.
TCG explicitly skips unavailable CPU PMU discovery. Ordinary runs remain four
vCPUs. The underlying helper accepts `LINUX_TEST_KERNEL` for immutable before
images and `LINUX_TEST_SMP` (1–64) for a direct regression reproducer.

Raw run logs and complete source-snapshot manifests are retained locally in
ignored evidence directories; they are not included in the source checkout.
The documented commands regenerate these artifacts.
