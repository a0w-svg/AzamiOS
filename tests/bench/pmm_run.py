#!/usr/bin/env python3
"""Measure pinned allocation rounds and approximate free-versus-buddy retention."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests/semantics"))
from namespaces import command, digest, execute, save, source_state


def observe(path, require_metadata):
    text = path.read_text(errors="replace")
    if re.search(r"PANIC|Triple fault|Kernel panic|divergence|PMMFAIL|PMMSKIP", text):
        raise ValueError("fatal diagnostic or incomplete allocation workload")
    records = {}
    for prefix in ("PMMENV", "PMMBENCH", "PMMSNAPSHOT"):
        records[prefix] = [json.loads(line[len(prefix) + 1:]) for line in text.splitlines()
                           if line.startswith(prefix + " ")]
    rows, snapshots = records["PMMBENCH"], records["PMMSNAPSHOT"]
    verdict = re.findall(r"== probe complete: ([0-9]+) passed, ([0-9]+) failed ==", text)
    if (len(records["PMMENV"]) != 1 or len(rows) != 55 or len(snapshots) != 66 or
            verdict != [("55", "0")]):
        raise ValueError("missing/inconsistent allocation records or verdict")
    if any(not isinstance(r, dict) for records_of_kind in records.values() for r in records_of_kind):
        raise ValueError("non-object measurement record")
    actual_order = []
    for line in text.splitlines():
        prefix = line.split(" ", 1)[0]
        if prefix not in records:
            continue
        record = json.loads(line[len(prefix) + 1:])
        detail = record.get("phase") if prefix == "PMMSNAPSHOT" else record.get("worker")
        actual_order.append((prefix, record.get("workers"), record.get("repetition"), detail))
    expected_order = [("PMMENV", None, None, None)]
    for workers in (1, 4):
        for rep in range(-1, 10):
            expected_order += [("PMMSNAPSHOT", workers, rep, phase) for phase in ("before", "held", "after")]
            expected_order += [("PMMBENCH", workers, rep, worker) for worker in range(workers)]
    if actual_order != expected_order:
        raise ValueError("allocation barriers/snapshot records out of order")
    environment = records["PMMENV"][0]
    if (environment.get("mapping_bytes_per_worker") != 16777216 or
            environment.get("pages_per_worker") != 4096 or environment.get("page_bytes") != 4096 or
            environment.get("warmup_rounds") != 1 or environment.get("measured_rounds") != 10 or
            type(environment.get("round_timeout_ns")) is not int or environment["round_timeout_ns"] != 30000000000 or
            type(environment.get("total_timeout_ns")) is not int or environment["total_timeout_ns"] != 300000000000 or
            type(environment.get("timer_resolution_ns")) is not int or environment["timer_resolution_ns"] <= 0 or
            not isinstance(environment.get("cpus"), list) or len(environment["cpus"]) != 4 or
            any(type(cpu) is not int or cpu < 0 for cpu in environment["cpus"]) or
            len(set(environment["cpus"])) != 4):
        raise ValueError("invalid environment/count/timer evidence")
    for r in rows:
        if any(type(r.get(k)) is not int for k in ("workers", "repetition", "worker", "cpu", "unmap_cpu")):
            raise ValueError("invalid worker identity")
    summaries = {}
    for workers in (1, 4):
        timings = {k: [] for k in ("mmap_ns", "touch_ns", "unmap_ns", "map_touch_ns")}
        estimates = {k: [] for k in ("before", "held", "after")}
        for rep in range(-1, 10):
            group = [r for r in rows if r.get("workers") == workers and r.get("repetition") == rep]
            if (len(group) != workers or {r.get("worker") for r in group} != set(range(workers)) or
                    len({r.get("requested_cpu") for r in group}) != workers):
                raise ValueError("duplicate/missing workers or requested CPUs")
            for r in group:
                checksum = sum((page + r["worker"] * 17) % 251 + 1 for page in range(4096))
                if (r.get("passed") is not True or r.get("pages") != 4096 or
                        r.get("bytes") != 16777216 or r.get("checksum") != checksum or
                        type(r.get("requested_cpu")) is not int or r["requested_cpu"] < 0 or
                        r.get("cpu") != r["requested_cpu"] or r.get("unmap_cpu") != r["requested_cpu"] or
                        r["requested_cpu"] != environment["cpus"][r["worker"]] or
                        any(type(r.get(k)) is not int or r[k] <= 0 for k in ("mmap_ns", "touch_ns", "unmap_ns"))):
                    raise ValueError("invalid counted work, contents, affinity or phase timing")
            if rep >= 0:
                for key in ("mmap_ns", "touch_ns", "unmap_ns"):
                    timings[key].append(max(r[key] for r in group))
                timings["map_touch_ns"].append(max(r["mmap_ns"] + r["touch_ns"] for r in group))
            for phase in ("before", "held", "after"):
                selected = [s for s in snapshots if s.get("workers") == workers and
                            s.get("repetition") == rep and s.get("phase") == phase]
                if len(selected) != 1:
                    raise ValueError("duplicate/missing parked-worker snapshot")
                s = selected[0]
                flags = []
                for name in ("memfree", "buddy"):
                    status = s.get(name + "_status"); value = s.get(name + "_bytes")
                    if status not in ("ok", "absent", "unparseable", "read_error"):
                        raise ValueError("invalid metadata status")
                    if (status == "ok" and (type(value) is not int or value < 0)) or (status != "ok" and value is not None):
                        raise ValueError("invalid metadata value")
                    flags.append(status == "ok")
                available = "ok" if all(flags) else "partial" if any(flags) else "unavailable"
                estimate = s["memfree_bytes"] - s["buddy_bytes"] if all(flags) else None
                if (s.get("available") != available or s.get("estimate") != estimate or
                        (estimate is not None and type(s["estimate"]) is not int) or
                        (require_metadata and available != "ok")):
                    raise ValueError("unavailable/inconsistent allocator metadata")
                if rep >= 0 and estimate is not None:
                    estimates[phase].append(estimate)
        summaries[str(workers)] = {}
        for key, samples in timings.items():
            median = statistics.median(samples)
            summaries[str(workers)][key] = {"median": median, "min": min(samples), "max": max(samples),
                                           "mad": statistics.median(abs(s - median) for s in samples)}
        summaries[str(workers)]["approximate_cache_bytes"] = {
            key: {"median": statistics.median(values), "min": min(values), "max": max(values)}
            if values else None for key, values in estimates.items()}
    return {"environment": records["PMMENV"][0], "workers": rows, "snapshots": snapshots,
            "summary": summaries, "scope": "Order-0 mapping and content integrity; cross-read retention estimate, no starvation/mixed-order proof."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    work = args.output.resolve()
    if not work.is_relative_to((REPO / "build").resolve()):
        parser.error("output must be a new directory under build/")
    work.mkdir(parents=True, exist_ok=False)
    kernel = REPO / "build/kernel.elf"; payload = REPO / "build/bench/bin/bench_pmm"
    manifest = {"started_utc": datetime.now(timezone.utc).isoformat(), "status": "running",
                "source": source_state(), "kernel_sha256": digest(kernel), "payload_sha256": digest(payload),
                "compiler": command([str(REPO / "tools/linux/musl/bin/musl-gcc"), "--version"]),
                "qemu": command(["qemu-system-x86_64", "--version"]),
                "flags": "-static -std=c11 -O2 -g -Wall -Wextra -Werror",
                "guest": {"smp": 4, "ram": "1536M", "machine": "q35", "firmware": "bios",
                          "accelerator": "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg"},
                "config": {"workers": [1, 4], "bytes_per_worker": 16777216, "warmups": 1, "repetitions": 10}}
    manifest["timeout_seconds"] = {"linux": 150, "guest": 150, "guest_outer": 180}
    save(work / "manifest.json", manifest)
    try:
        execute([str(payload)], work / "linux.txt", 150)
        host = observe(work / "linux.txt", False)
        env = dict(os.environ, LINUX_TEST_WORK=str(work / "vm"), LINUX_TEST_TIMEOUT="150",
                   LINUX_TEST_KERNEL=str(kernel), LINUX_TEST_SMP="4")
        execute([str(REPO / "scripts/linux-test.sh"), str(payload)], work / "guest.txt", 180, env)
        guest = observe(work / "vm/serial.log", True)
        save(work / "observations.json", {"linux": host, "azami": guest})
        manifest.update(status="passed", iso_sha256=digest(work / "vm/test.iso"))
        print(json.dumps(guest["summary"], indent=2)); print("PASS:", work)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        manifest.update(status="failed", error=str(error)); print("FAIL:", error, file=sys.stderr); return 1
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat(); save(work / "manifest.json", manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
