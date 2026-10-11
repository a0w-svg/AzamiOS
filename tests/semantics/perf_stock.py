#!/usr/bin/env python3
"""Run pinned upstream perf stat and require actual counts and PMU wrap evidence."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from namespaces import REPO, command, digest, execute, parse, save, source_state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    parent = REPO / "build/semantics"
    parent.mkdir(parents=True, exist_ok=True)
    if args.output:
        work = args.output.resolve()
        if not work.is_relative_to((REPO / "build").resolve()):
            parser.error("--output must be a new directory under build/")
        work.mkdir(parents=True, exist_ok=False)
    else:
        work = Path(tempfile.mkdtemp(prefix="perf-stock-", dir=parent))
    perf = REPO / "tools/linux/out/bin/perf"
    payload = REPO / "tools/linux/out/bin/azami-perf-stat-probe"
    root = work / "root"
    (root / "usr/bin").mkdir(parents=True)
    shutil.copy2(perf, root / "usr/bin/perf")
    manifest = {"started_utc": datetime.now(timezone.utc).isoformat(), "status": "running",
                "source": source_state(), "kernel_sha256": digest(REPO / "build/kernel.elf"),
                "payload_sha256": digest(payload), "perf_sha256": digest(perf),
                "perf_version": command([str(perf), "version"]),
                "upstream_archive_sha256": digest(REPO / "tools/linux/dl/linux-6.12.tar.xz"),
                "suite": "perf-stock", "timeout_seconds": 40,
                "guest": {"firmware": "bios", "machine": "q35", "smp": 4, "ram": "1536M",
                          "accelerator": "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg",
                          "cmdline": "azami.pmu_selftest=1"},
                "qemu": command(["qemu-system-x86_64", "--version"])}
    save(work / "manifest.json", manifest)
    print(f"stock perf and PMU wrap: {work}", flush=True)
    try:
        guest = [str(REPO / "scripts/linux-test.sh"), str(payload)]
        manifest["guest_command"] = guest
        env = dict(os.environ, LINUX_TEST_WORK=str(work / "vm"), LINUX_TEST_TIMEOUT="40",
                   LINUX_TEST_ROOT=str(root), LINUX_TEST_CMDLINE="azami.pmu_selftest=1",
                   LINUX_TEST_KERNEL=str(REPO / "build/kernel.elf"), LINUX_TEST_SMP="4")
        execute(guest, work / "guest.txt", 70, env)
        observed = parse(work / "guest.txt")
        records = []
        for line in (work / "vm/serial.log").read_text(errors="replace").splitlines():
            if line.startswith("PMUTEST ") or line.startswith("PMUSKIP "):
                records.append((line.split(" ", 1)[0], json.loads(line.split(" ", 1)[1])))
        if len(records) != 1:
            raise ValueError("exactly one PMU selftest result required")
        kind, record = records[0]
        required = {"stock.worker"} | {f"stock.task-clock.{op}" for op in ("exit", "log.complete", "positive")}
        if kind == "PMUTEST":
            width = record.get("width")
            if (record.get("passed") is not True or type(width) is not int or not 32 <= width <= 64 or
                    not (1 << width)-4097 < record["before"] <= (1 << width)-1 or
                    not 0 <= record["after"] < record["before"] or
                    not 65536 <= record["delta"] < 1048576 or
                    record["delta"] != (record["after"]-record["before"]) & ((1 << width)-1)):
                raise ValueError("hardware wrap selftest failed")
            required |= {f"stock.{event}.{op}" for event in ("instructions", "raw")
                         for op in ("exit", "log.complete", "positive")}
            if observed["skips"]:
                raise ValueError("present PMU must count stock hardware events")
        elif (not isinstance(record.get("reason"), str) or not record["reason"] or
              observed["skips"] != [{"name": "stock.instructions", "reason": "PMU unavailable or permission denied"}]):
            raise ValueError("unavailable PMU must have exactly one hardware skip")
        if not required <= observed["cases"].keys():
            raise ValueError("stock perf counting coverage missing")
        save(work / "observations.json", {"stock": observed, "wrap": record, "wrap_kind": kind})
        manifest.update(status="passed", guest_passed=observed["passed"], guest_skips=observed["skips"],
                        pmu_selftest=record, iso_sha256=digest(work / "vm/test.iso"))
        print(f"PASS: {observed['passed']} stock-tool observations; {kind}")
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        manifest.update(status="failed", error=str(error))
        print(f"FAIL: {error}; logs: {work}", file=sys.stderr)
        return 1
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save(work / "manifest.json", manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
