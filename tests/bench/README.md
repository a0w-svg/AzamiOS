# W4.1 reproducible benchmarks

The four static musl binaries run unchanged on Linux and AzamiOS. The suite
supervisor executes one complete warm-up round, then ten measured rounds by
default. Each process also warms its measured operation before starting its
timer. All results use checked `CLOCK_MONOTONIC` timestamps; CPU counters and
sampling are outside this milestone.

```sh
make -C tools/linux toolchain
make -C tests/bench all check
tests/bench/run.sh --target linux
tests/bench/run.sh --target guest --accelerator tcg
tests/bench/run.sh --target guest --accelerator kvm
tests/bench/run.sh --target guest --firmware uefi --smp 1 --ram 512M
```

`run.sh` builds the suite; guest runs also rebuild `build/kernel.elf`. A guest
gets a private ext2 root and ISO, no attached user disks, and no networking.
All output goes into a new directory under `build/bench/results/`; choose a new
explicit path with `--output build/bench/my-run`. Existing result directories
are never reused. Requested unavailable KVM runs record a skip and exit 77.
UEFI requires installed OVMF firmware and Limine EFI files.

Defaults and smaller correctness runs:

| Option | Default | Meaning |
| --- | --- | --- |
| `--syscall-iterations` | 10000000 | Raw x86-64 `getpid` calls, checking every result |
| `--pipe-iterations` | 10000 | One-byte request/reply round trips across two pipes |
| `--mmap-mib` | 256 | Size of each fresh anonymous mapping |
| `--fork-iterations` | 1000 | `fork` plus checked `waitpid` |
| `--warmups` | 1 | Discarded full suite rounds |
| `--repetitions` | 10 | Measured suite rounds |
| `--case-timeout` | 120 | Per-process deadline in guest/host monotonic seconds |
| `--timeout` | 1800 | Independent host deadline in seconds, including guest boot |
| `--smp`, `--ram` | 4, 1536M | Guest vCPUs and RAM |
| `--cpu` | max with rdrand/rdseed (TCG), host (KVM) | Exact QEMU CPU argument |

```sh
tests/bench/run.sh --target guest --syscall-iterations 10000 \
    --pipe-iterations 100 --mmap-mib 4 --fork-iterations 10 --repetitions 1
build/bench/bin/bench_syscall 10000
build/bench/bin/bench_pipe 100
build/bench/bin/bench_mmap 4
build/bench/bin/bench_fork 10
```

The standalone argument is the iteration count, or MiB for `bench_mmap`.
Arguments are positive bounded integers. Runs with fewer than ten measured
repetitions are marked `baseline_eligible: false`.

The pipe child validates the request token and returns its complement; the
parent checks each reply, EOF, and child exit. Short transfers and EINTR are
handled explicitly. The reported latency covers the whole round trip; it is
not one context switch. The fork benchmark checks every child and reaps it.
The raw syscall loop includes result verification and a consumed checksum, so
the reported duration includes that small user-loop cost.

The memory benchmark precomputes a Fisher–Yates permutation with fixed
xorshift seed `4d595df4d0f33173`, outside its measured interval. Sequential and
shuffled writes each use a fresh mapping. It verifies the stored byte on every
page and that adjacent and final bytes stay zero, then unmaps the region.

**Memory measurement correction:** AzamiOS currently allocates anonymous
pages eagerly in `sys_mmap_impl`; Linux commonly faults them on first write.
Therefore `elapsed_ns` is the sum of mapping and first-touch durations, with
`mmap_elapsed_ns` and `touch_elapsed_ns` also retained. The rates describe
logical pages/MiB provisioned and touched per second, including allocation
wherever the kernel performs it. They do not count actual page faults and are
not streaming memory bandwidth. Transparent huge-page policy is left at each
system's default; actual hardware fault counts are not inferred from page size.
This correction avoids claiming a speedup from moving allocation before the
timer. Changing anonymous mapping policy belongs to a later kernel milestone.

Each run saves:

- `manifest.json`: target, accelerator, actual CPU brand and kernel identity,
  reported timer resolution, exact counts, compiler/version/flags, payload
  hashes, Git revision, dirty/untracked source content fingerprint and file
  hashes, host affinity/load, and the full command. Guest manifests also hash
  the kernel, boot assets, initrd, and ISO and record firmware/SMP/RAM.
- `serial.log` and `qemu.log` (guest) or `serial.log` (Linux): complete output;
  `prepare.log` records guest build/image creation. Host CPU/memory/kernel
  information is stored alongside the manifest.
- `records.jsonl`: raw protocol, including warm-ups and checked child statuses.
  `samples.jsonl`: validated measurements tagged by case/repetition/warm-up.
- `summary.json`: median, min/max, interquartile range, and median absolute
  deviation of measured durations and rates. Warm-up samples are excluded.

The line protocol is `AZBENCH ` followed by a JSON object. An `environment`
record precedes ordered `begin`, `sample`, and `end` records for each child;
`suite_end` terminates the run. Each child contributes one sample, except mmap
which contributes sequential and shuffled samples. All durations/counts are
unsigned integer values in nanoseconds/operations. The host verifies exact
case counts/order, variants, checksums, durations, timer resolution, warm-ups,
and zero child statuses. A failed child, missing result/verdict, malformed
record, fatal kernel diagnostic, premature VM exit, or either deadline makes
the run fail. The supervisor kills the failed child's process group and reaps
it; SIGTERM/SIGINT also stop and reap the active group. The host independently
terminates the VM/supervisor session on failure or interruption.

`make check` exercises all real workloads on Linux and the runner's failure
paths, including nonzero/missing-output children and child/host timeouts. Logs
for those cases remain under `build/bench/runner-tests/`.

Keep Linux-host, TCG, KVM, and physical-hardware records separate. TCG validates
execution and can expose gross regressions; it does not establish physical CPU
performance. Compare unchanged workload counts and machine configurations,
use at least ten measured repetitions, and adopt an optimization only when its
improvement exceeds the recorded noise without unexplained regressions in the
other workloads. A Linux-host run is a compatibility comparison, not an
equal-hardware comparison with the TCG guest.

See [the W4.1 execution record](../../docs/W4.1_BENCHMARK_RESULTS.md) for the
saved baseline and regression gates from this implementation.

The W4.3 descriptor benchmark runs 20,000 checked eventfd write/read pairs per
worker in one-thread, four-thread and four-process modes. Each worker checks
affinity installation, warms up 256 pairs before timing, and verifies every
transfer and final checksum. One full round is discarded; ten are measured.
The CPU field is the requested affinity CPU. The summary reports the median
slowest-worker duration per pair with min/max and MAD; independent worker timers
do not measure group makespan or aggregate throughput.

```sh
make -C tests/bench all
python3 -B tests/bench/fd_run.py --kernel build/kernel.elf \
    --output build/bench/fd-current
```

The runner records the immutable selected ELF separately from the worktree at
launch. For comparisons using a saved kernel, pass `--kernel-source-manifest`
pointing to a matching semantic-run manifest; it verifies the ELF hash and
retains that origin's source snapshot. Use the same payload, counts and guest
configuration in both runs. See [W4.3 results](../../docs/W4.3_FD_RESULTS.md).

The controlled allocator baseline uses persistent fork workers with private
memory/FD tables and verified CPU affinity. It maps 16 MiB per worker, checks
all 4,096 page markers and untouched bytes, holds mappings at a barrier, then
unmaps them. There is one discarded round and ten measured rounds for one and
four workers. Worker timings cover mmap, first touch and unmap separately;
map+touch includes eager allocation. Per-round procfs snapshots record MemFree
and weighted buddy counts while workers are parked. Their difference is an
approximate cross-read retention estimate, not exact per-CPU cache attribution.

```sh
python3 -B tests/bench/pmm_run.py --output build/bench/pmm-current
python3 -B tests/bench/test_pmm_runner.py \
    tests/bench/baselines/2026-10-07/pmm/kvm/linux.txt
```

The runner requires all 55 worker records and 66 ordered snapshots, actual CPU
identity, checksums, positive phase durations, complete guest metadata and final
verdict. Missing Linux metadata is explicitly recorded. Negative retention
estimates are allowed because the reads are separate. No allocator tuning or
starvation/mixed-order proof follows from this workload; see
[the measurement record](../../docs/W4.3_PMM_RESULTS.md).
