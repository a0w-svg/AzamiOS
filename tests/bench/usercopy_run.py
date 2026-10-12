#!/usr/bin/env python3
"""Measure complete memfd I/O pairs; timings are not raw kernel-copy bandwidth."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests/semantics"))
from namespaces import execute

SIZES = (16, 64, 256, 4096, 65536, 1048576)
ORDER = [(repetition, size) for repetition in range(-1, 10) for size in SIZES]
SCOPE = ("End-to-end pinned memfd lseek/write/lseek/read pairs and content checks; "
         "includes syscall, filesystem and checking costs, not raw kernel-copy bandwidth or a speedup claim.")


def iterations(size):
    return min(4096, max(4, 4194304 // size))


def checksum(size):
    # Multiplication by 17 permutes the residues modulo 251.
    complete, tail = divmod(size, 251)
    return complete * 251 * 250 // 2 + sum((i * 17 + size) % 251 for i in range(tail))


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON field")
        result[key] = value
    return result


def distribution(samples):
    median = statistics.median(samples)
    return dict(median=median, min=min(samples), max=max(samples),
                mad=statistics.median(abs(sample - median) for sample in samples))


def observe(path):
    text = Path(path).read_text(errors="replace")
    if re.search(r"PANIC|Triple fault|Kernel panic|COPYFAIL|COPYSKIP|divergence", text):
        raise ValueError("fatal diagnostic or incomplete copy workload")
    environments, rows, sequence, verdicts = [], [], [], []
    for line in text.splitlines():
        if line.startswith("COPYENV") or line.startswith("COPYBENCH"):
            prefix, separator, encoded = line.partition(" ")
            if not separator or prefix not in ("COPYENV", "COPYBENCH"):
                raise ValueError("malformed copy record prefix")
            record = json.loads(encoded, object_pairs_hook=unique_object)
            if type(record) is not dict:
                raise ValueError("non-object copy record")
            (environments if prefix == "COPYENV" else rows).append(record)
            sequence.append(prefix)
        elif line.startswith("== probe complete:"):
            verdicts.append(line)
            sequence.append("verdict")
    if (len(environments) != 1 or len(rows) != len(ORDER) or
            sequence != ["COPYENV"] + ["COPYBENCH"] * len(ORDER) + ["verdict"] or
            verdicts != ["== probe complete: 66 passed, 0 failed =="]):
        raise ValueError("missing, duplicate, reordered or failed benchmark/verdict")
    environment = environments[0]
    if (set(environment) != {"cases", "warmups", "repetitions", "cpu", "timer_resolution_ns"} or
            any(type(value) is not int for value in environment.values()) or
            environment["cases"] != 66 or environment["warmups"] != 1 or
            environment["repetitions"] != 10 or not 0 <= environment["cpu"] < 2**31 or
            not 0 < environment["timer_resolution_ns"] < 2**63):
        raise ValueError("invalid counted work, CPU or timer environment")
    fields = {"size_bytes", "repetition", "iterations", "elapsed_ns", "checksum", "canary"}
    for row, (repetition, size) in zip(rows, ORDER):
        if (set(row) != fields or row["canary"] is not True or
                any(type(row[key]) is not int for key in fields - {"canary"}) or
                row["repetition"] != repetition or row["size_bytes"] != size or
                row["iterations"] != iterations(size) or row["checksum"] != checksum(size) or
                not 0 < row["elapsed_ns"] < 2**63):
            raise ValueError("invalid ordering, work, contents, canary or timing")
    summary = {}
    for size in SIZES:
        measured = [row for row in rows if row["size_bytes"] == size and row["repetition"] >= 0]
        summary[str(size)] = dict(samples=len(measured), iterations=iterations(size),
            ns_per_pair=distribution([row["elapsed_ns"] / row["iterations"] for row in measured]),
            logical_mib_per_second=distribution([2 * size * row["iterations"] * 1e9 /
                                                 (row["elapsed_ns"] * 1048576) for row in measured]))
    return dict(environment=environment, rows=rows, summary=summary, scope=SCOPE)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save_summary(path, observations):
    # Compact evidence preserves every row without including raw logs/images.
    sections = []
    for platform, observed in observations.items():
        header = {key: value for key, value in observed.items() if key != "rows"}
        encoded = json.dumps(header, indent=2)[:-2] + ',\n  "rows": [\n'
        encoded += ",\n".join("    " + json.dumps(row, separators=(",", ":")) for row in observed["rows"])
        encoded += "\n  ]\n}"
        sections.append("  " + json.dumps(platform) + ": " + encoded.replace("\n", "\n  "))
    path.write_text("{\n" + ",\n".join(sections) + "\n}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=REPO / "build/kernel.elf")
    parser.add_argument("--payload", type=Path, default=REPO / "build/bench/bin/bench_usercopy")
    parser.add_argument("--accel", nargs="+", choices=("kvm", "tcg"), default=["tcg"])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--linux-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = REPO / "tests/bench/bench_usercopy.c"
    kernel_sources = ("kernel/syscall/sys_fs.c", "kernel/syscall/sys_mm.c", "fs/io_iter.h", "fs/io_vec.h",
                      "fs/pipe.c", "fs/pipe.h", "fs/vfs.h", "fs/tmpfs.c", "fs/fat32.c",
                      "fs/ext2/ext2.c", "fs/squashfs/squashfs.c", "arch/x86_64/lib/uaccess.asm")
    source_hashes = {name: digest(REPO / name) for name in kernel_sources if (REPO / name).is_file()}
    source_hashes["tests/bench/bench_usercopy.c"] = digest(source)
    source_hashes["tests/bench/common.h"] = digest(REPO / "tests/bench/common.h")
    source_hashes["tests/bench/usercopy_run.py"] = digest(Path(__file__))
    source_hashes["tests/bench/test_usercopy_run.py"] = digest(REPO / "tests/bench/test_usercopy_run.py")
    manifest = dict(started_utc=datetime.now(timezone.utc).isoformat(), status="running", scope=SCOPE,
                    source_sha256=source_hashes, linux_release=os.uname().release,
                    host_affinity=sorted(os.sched_getaffinity(0)), host_load_start=list(os.getloadavg()),
                    host_cpu_model=next((line.split(":", 1)[1].strip() for line in
                                         Path("/proc/cpuinfo").read_text().splitlines()
                                         if line.startswith("model name") and ":" in line), None),
                    source_revision=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                    config=dict(sizes=list(SIZES), warmups=1, repetitions=10, cases=66),
                    timeout_seconds=dict(linux=90, guest=90, guest_outer=120))
    manifest_path = output / "manifest.json"
    observations = {}
    try:
        payload = output / "probe"
        payload.write_bytes(args.payload.read_bytes()); payload.chmod(0o700)
        manifest["payload_sha256"] = digest(payload)
        # build_info.py writes <binary>.json, containing actual flags/version.
        build_metadata_path = Path(str(args.payload) + ".json")
        if build_metadata_path.is_file():
            build_metadata = json.loads(build_metadata_path.read_text())
            if build_metadata.get("binary_sha256") != manifest["payload_sha256"]:
                raise ValueError("build metadata does not describe the measured payload")
            for name, expected in build_metadata.get("sources", {}).items():
                candidate = REPO / "tests/bench" / name
                if not candidate.is_file() or digest(candidate) != expected:
                    raise ValueError("build source metadata mismatch: " + name)
            manifest.update(build_metadata_status="present", build=build_metadata)
        else:
            manifest["build_metadata_status"] = "absent"
        kernel = None
        if not args.linux_only:
            kernel = output / "kernel.elf"
            kernel.write_bytes(args.kernel.read_bytes())
            manifest.update(kernel_sha256=digest(kernel),
                            guest=dict(machine="q35", cpus=4, memory_mb=1536, net="none", firmware="bios"))
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        execute([str(payload)], output / "linux.txt", 90, dict(os.environ, USERCOPY_BENCH_EXIT="1"))
        observations["linux"] = observe(output / "linux.txt")
        if observations["linux"]["environment"]["cpu"] not in manifest["host_affinity"]:
            raise ValueError("measured Linux CPU outside inherited affinity")
        if kernel:
            for accel in dict.fromkeys(args.accel):
                work = output / accel
                env = dict(os.environ, LINUX_TEST_WORK=str(work), LINUX_TEST_KERNEL=str(kernel),
                           LINUX_TEST_ACCEL=accel, LINUX_TEST_SMP="4", LINUX_TEST_MEM_MB="1536",
                           LINUX_TEST_NET="none", LINUX_TEST_TIMEOUT="90",
                           LINUX_TEST_CMDLINE=os.environ.get("LINUX_TEST_CMDLINE", "quiet"))
                execute([str(REPO / "scripts/linux-test.sh"), str(payload)], output / (accel + ".txt"), 120, env)
                observations[accel] = observe(work / "serial.log")
                if observations[accel]["environment"]["cpu"] >= 4:
                    raise ValueError("guest affinity CPU outside configured topology")
                manifest.setdefault("guest_iso_sha256", {})[accel] = digest(work / "test.iso")
                print(f"{accel}: 66 validated records; 10 measured rounds per size", flush=True)
        for name, expected in source_hashes.items():
            if digest(REPO / name) != expected:
                raise ValueError("source changed during measurement: " + name)
        save_summary(output / "summary.json", observations)
        manifest.update(status="passed", platforms=list(observations),
                        environment={name: observed["environment"] for name, observed in observations.items()})
        print("PASS end-to-end copy benchmark:", output, flush=True)
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        manifest.update(status="failed", error=str(error))
        print("FAIL:", error, file=sys.stderr)
        return 1
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        manifest["host_load_end"] = list(os.getloadavg())
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
