#!/usr/bin/env python3
"""Run opt-in boot PMM exhaustion tests and reject incomplete evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess

REPO = Path(__file__).resolve().parents[2]
PHASES = ["mixed", "exhaust", "remote", "restore", "PASS"]


def observe(path, cpus, drop_cpu=None, worker_probe=False):
    if cpus not in range(1, 5) or (drop_cpu is not None and (drop_cpu != 2 or cpus != 4)):
        raise ValueError("drop CPU 2 requires four CPUs")
    text = Path(path).read_text(errors="replace")
    if re.search(r"PANIC|Triple fault|Kernel panic|divergence|double.free", text):
        raise ValueError("fatal allocator/boot diagnostic")
    records, workers, boot_positions, worker_positions = [], [], [], []
    lines = text.splitlines()
    for position, line in enumerate(lines):
        if line.startswith("PMMTEST "):
            record = json.loads(line[8:])
            if not isinstance(record, dict) or record.get("passed") is not True:
                raise ValueError("failed, skipped or malformed PMM record")
            records.append(record)
            boot_positions.append(position)
        elif line.startswith("PMMWORKER "):
            workers.append(json.loads(line[10:]))
            worker_positions.append(position)
    if [r.get("phase") for r in records] != PHASES:
        raise ValueError("missing, repeated or reordered PMM phases")
    mixed, exhaust, remote, restore, verdict = records

    def number(record, key, expected=None, low=0, high=2**63-1):
        value = record.get(key)
        if type(value) is not int or not low <= value <= high:
            raise ValueError("invalid " + key)
        if expected is not None and value != expected:
            raise ValueError("unexpected " + key)
        return value

    number(verdict, "cpus", cpus, 1, 4)
    number(mixed, "allocations", 30)
    number(mixed, "orders_mask", 1023)
    number(mixed, "pages", 3069)
    number(mixed, "zero_bytes", 1023 * 4096)
    number(exhaust, "allocations", low=1, high=4096)
    number(exhaust, "orders_mask", low=1, high=(1 << 19)-1)
    held = number(exhaust, "pages", low=1, high=524288)
    number(exhaust, "order_failures", 19)
    number(exhaust, "buddy_pages", 0)
    cached = int(cpus > 1)
    number(exhaust, "cached_pages", cached)
    if remote.get("supported") is not (cpus > 1) or remote.get("global_oom") is not True:
        raise ValueError("invalid remote/global exhaustion result")
    number(remote, "cpu", cached)
    number(remote, "cached_pages", cached)
    number(remote, "drained", cached)
    number(remote, "pressure_zones", 3)
    number(remote, "dma32_pressure_zones", 1)
    if remote.get("irq_disabled_failure") is not True:
        raise ValueError("missing IRQ-disabled allocator failure coverage")
    number(remote, "sweeps", 1)
    number(remote, "reclaimed_pages", cached)
    number(remote, "callbacks", cpus - int(drop_cpu is not None))
    number(remote, "rpc_timeouts", int(drop_cpu is not None))
    number(remote, "busy", 0)
    number(remote, "cpu_budget", 8)
    number(remote, "page_budget", 256)
    number(remote, "callback_budget", 32)
    seed = number(remote, "seed", low=4096 if cached else 0)
    number(remote, "recovered", seed)
    if seed % 4096 or (not cached and seed):
        raise ValueError("unaligned or unexpected seed")
    initial = number(restore, "initial_pages", held + cached)
    number(restore, "restored_pages", initial)
    number(restore, "cached_pages", 0)
    number(restore, "local_drained", high=256)
    if restore.get("orders_restored") is not True:
        raise ValueError("buddy orders not restored")
    # Completing boot into an independent payload checks subsequent usability.
    expected_verdict = f"== probe complete: {8 if worker_probe else 121} passed, 0 failed =="
    verdict_positions = [i for i, line in enumerate(lines)
                         if re.fullmatch(r"== probe complete: .* ==", line)]
    if (len(verdict_positions) != 1 or lines[verdict_positions[0]] != expected_verdict or
            verdict_positions[0] <= boot_positions[-1]):
        raise ValueError("missing or repeated post-test ABI verdict")
    worker_verdict_positions = [i for i, line in enumerate(lines)
                               if line.startswith("== PMM worker probe complete:")]
    if not worker_probe:
        if workers or worker_verdict_positions:
            raise ValueError("unexpected worker evidence")
        return records
    if (len(workers) != 1 or not isinstance(workers[0], dict) or
            len(worker_verdict_positions) != 1 or
            lines[worker_verdict_positions[0]] != "== PMM worker probe complete: 0 failed ==" or
            not boot_positions[-1] < worker_positions[0] < worker_verdict_positions[0] < verdict_positions[0]):
        raise ValueError("missing, repeated or reordered worker evidence")
    worker = workers[0]
    number(worker, "worker_started", 1)
    quiescent_cycles = number(worker, "quiescent_cycles", low=10)
    number(worker, "worker_cycles", low=quiescent_cycles + 2)
    worker_sweeps = number(worker, "worker_sweeps", low=1, high=8)
    number(worker, "quiescent_sweeps", worker_sweeps)
    number(worker, "pressure_batches", low=remote["sweeps"] + 1)
    number(worker, "sweeps", low=remote["sweeps"] + 1)
    number(worker, "callbacks", low=remote["callbacks"] + 1)
    number(worker, "reclaimed_pages", low=remote["reclaimed_pages"])
    number(worker, "rpc_timeouts", low=remote["rpc_timeouts"])
    for field in ("lock_busy", "rpc_busy", "offline_cpus"):
        number(worker, field)
    number(worker, "start_failures", 0)
    number(worker, "time_unavailable", 0)
    number(worker, "pending_zones", 0)
    records.append(worker)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accel", choices=("kvm", "tcg"), required=True)
    parser.add_argument("--cpus", nargs="+", type=int, choices=range(1, 5), default=[1, 4])
    parser.add_argument("--memory", nargs="+", type=int, choices=range(256, 2049), metavar="MIB", default=[512, 1536],
                        help="QEMU MiB; 256 minimum leaves at least 128MiB managed for the boot guard")
    parser.add_argument("--drop-cpu", type=int, choices=(2,), help="drop CPU 2 RPCs; requires --cpus 4")
    parser.add_argument("--worker-probe", action="store_true", help="validate actual deferred worker execution")
    parser.add_argument("--kernel", type=Path, default=REPO / "build/kernel.elf",
                        help="kernel ELF to copy once into the output directory")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.drop_cpu is not None and set(args.cpus) != {4}:
        parser.error("--drop-cpu 2 requires --cpus 4")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    kernel = args.kernel.resolve()
    kernel_bytes = kernel.read_bytes()
    digest = hashlib.sha256(kernel_bytes).hexdigest()
    source = REPO / "kernel/mm/pmm.c"
    source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
    immutable = output / "kernel.elf"
    immutable.write_bytes(kernel_bytes)
    cmdline = "azami.pmm_selftest=1"
    if args.drop_cpu is not None:
        cmdline += f" smp_rpc_drop_cpu={args.drop_cpu}"
    results = []
    for cpus in dict.fromkeys(args.cpus):
        for memory in dict.fromkeys(args.memory):
            work = output / f"{args.accel}-{cpus}cpu-{memory}m"
            env = dict(os.environ, LINUX_TEST_WORK=str(work), LINUX_TEST_KERNEL=str(immutable),
                       LINUX_TEST_CMDLINE=cmdline, LINUX_TEST_TIMEOUT="120",
                       LINUX_TEST_SMP=str(cpus), LINUX_TEST_MEM_MB=str(memory),
                       LINUX_TEST_ACCEL=args.accel, LINUX_TEST_NET="none")
            with (output / (work.name + ".txt")).open("w") as log:
                command = [str(REPO / "scripts/linux-test.sh")]
                if args.worker_probe:
                    command.append(str(REPO / "tools/linux/out/bin/azami-pmm-reclaim-probe"))
                process = subprocess.Popen(command, cwd=REPO,
                                           env=env, stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=True)
                try:
                    code = process.wait(timeout=150)
                    if code:
                        raise ValueError("guest runner failed: " + work.name)
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait()
            records = observe(work / "serial.log", cpus, args.drop_cpu, args.worker_probe)
            results.append(dict(accel=args.accel, cpus=cpus, memory_mb=memory,
                                network="none", drop_cpu=args.drop_cpu,
                                worker_probe=args.worker_probe, records=records))
            print("PASS", work.name, flush=True)
    if hashlib.sha256(source.read_bytes()).hexdigest() != source_digest:
        raise ValueError("allocator source changed during measurement")
    summary = dict(kernel_sha256=digest, pmm_sha256=source_digest,
                   configurations=results)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
