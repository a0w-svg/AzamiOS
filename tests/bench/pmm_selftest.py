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


def observe(path, cpus):
    text = Path(path).read_text(errors="replace")
    if re.search(r"PANIC|Triple fault|Kernel panic|divergence|double.free", text):
        raise ValueError("fatal allocator/boot diagnostic")
    records = []
    for line in text.splitlines():
        if line.startswith("PMMTEST "):
            record = json.loads(line[8:])
            if not isinstance(record, dict) or record.get("passed") is not True:
                raise ValueError("failed, skipped or malformed PMM record")
            records.append(record)
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
    # Completing the boot into an independent ABI probe checks that test
    # bookkeeping and page contents did not damage subsequent allocations.
    if len(re.findall(r"== probe complete: 121 passed, 0 failed ==", text)) != 1:
        raise ValueError("missing or repeated post-test ABI verdict")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accel", choices=("kvm", "tcg"), required=True)
    parser.add_argument("--cpus", nargs="+", type=int, choices=range(1, 5), default=[1, 4])
    parser.add_argument("--memory", nargs="+", type=int, choices=range(128, 2049), default=[512, 1536])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    kernel = REPO / "build/kernel.elf"
    digest = hashlib.sha256(kernel.read_bytes()).hexdigest()
    source = REPO / "kernel/mm/pmm.c"
    source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
    immutable = output / "kernel.elf"
    immutable.write_bytes(kernel.read_bytes())
    results = []
    for cpus in dict.fromkeys(args.cpus):
        for memory in dict.fromkeys(args.memory):
            work = output / f"{args.accel}-{cpus}cpu-{memory}m"
            env = dict(os.environ, LINUX_TEST_WORK=str(work), LINUX_TEST_KERNEL=str(immutable),
                       LINUX_TEST_CMDLINE="azami.pmm_selftest=1", LINUX_TEST_TIMEOUT="120",
                       LINUX_TEST_SMP=str(cpus), LINUX_TEST_MEM_MB=str(memory),
                       LINUX_TEST_ACCEL=args.accel, LINUX_TEST_NET="none")
            with (output / (work.name + ".txt")).open("w") as log:
                process = subprocess.Popen([str(REPO / "scripts/linux-test.sh")], cwd=REPO,
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
            records = observe(work / "serial.log", cpus)
            results.append(dict(accel=args.accel, cpus=cpus, memory_mb=memory,
                                network="none", records=records))
            print("PASS", work.name, flush=True)
    if hashlib.sha256(source.read_bytes()).hexdigest() != source_digest:
        raise ValueError("allocator source changed during measurement")
    summary = dict(kernel_sha256=digest, pmm_sha256=source_digest,
                   configurations=results)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
