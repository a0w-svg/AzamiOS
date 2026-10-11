#!/usr/bin/env python3
"""Run identical static payloads on Linux or a disposable AzamiOS VM."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
BIN = REPO / "build/bench/bin"
NAMES = ("syscall", "pipe", "mmap", "fork")
VARIANTS = {"syscall": ["getpid"], "pipe": ["round_trip"],
            "mmap": ["sequential", "shuffled"], "fork": ["fork_waitpid"]}
FATAL = re.compile(r"PANIC|Triple fault|Kernel panic|orders_mask/free_list divergence|free list/bitmap divergence")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True, allow_nan=False) + "\n")


def command(args, **kwargs):
    return subprocess.check_output(args, cwd=REPO, text=True, **kwargs).strip()


def source_state():
    # Include modified and untracked source contents, not only `git diff`.
    files = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=REPO)
    entries = {}
    for encoded in sorted(set(files.split(b"\0")) - {b""}):
        name = os.fsdecode(encoded)
        path = REPO / name
        if path.is_symlink():
            value = os.fsencode(os.readlink(path))
        elif path.is_file():
            value = path.read_bytes()
        else:
            value = b"<deleted>"
        entries[name] = {"sha256": hashlib.sha256(value).hexdigest(),
                         "mode": path.lstat().st_mode if path.exists() or path.is_symlink() else None}
    fingerprint = hashlib.sha256(json.dumps(entries, sort_keys=True).encode()).hexdigest()
    return {"revision": command(["git", "rev-parse", "HEAD"]),
            "status": command(["git", "status", "--porcelain=v1", "--untracked-files=all"]),
            "dirty_tree_sha256": fingerprint, "files": entries}


def integer(record, field, minimum=1):
    value = record.get(field)
    if type(value) is not int or value < minimum or value > 2**64 - 1:
        raise ValueError(f"invalid {field}: {value!r}")
    return value


def page_checksum(pages):
    blocks, tail = divmod(pages, 251)
    return blocks * (251 * 252 // 2) + tail * (tail + 1) // 2


def validate(records, config):
    """Strict ordered protocol; a successful exit alone is insufficient."""
    repetitions, warmups = config["repetitions"], config["warmups"]
    expected_cases = (repetitions + warmups) * 4
    samples = []
    active = None
    variants = []
    case = 0
    environment = None
    finished = False
    for record in records:
        if not isinstance(record, dict) or finished:
            raise ValueError("invalid record or output after suite_end")
        kind = record.get("type")
        if kind == "environment":
            if environment is not None or active is not None or case:
                raise ValueError("duplicate/misplaced environment")
            integer(record, "timer_resolution_ns")
            for field in ("sysname", "release", "machine", "cpu_brand", "clock"):
                if not isinstance(record.get(field), str):
                    raise ValueError(f"missing environment {field}")
            if record["clock"] != "CLOCK_MONOTONIC":
                raise ValueError("unexpected clock")
            environment = record
        elif kind == "begin":
            name = NAMES[case % 4]
            if environment is None or active is not None or case >= expected_cases:
                raise ValueError("unexpected begin")
            if (integer(record, "id", 0) != case or record.get("benchmark") != name or
                    type(record.get("warmup")) is not bool or
                    record["warmup"] != (case // 4 < warmups)):
                raise ValueError("case identity/warmup mismatch")
            active, variants = record, []
        elif kind == "sample":
            if active is None or record.get("benchmark") != active["benchmark"]:
                raise ValueError("sample outside its benchmark")
            name = active["benchmark"]
            if record.get("variant") not in VARIANTS[name] or record["variant"] in variants:
                raise ValueError("duplicate or unknown variant")
            iterations = integer(record, "iterations")
            integer(record, "elapsed_ns")
            resolution = integer(record, "timer_resolution_ns")
            if resolution != environment["timer_resolution_ns"]:
                raise ValueError("timer resolution changed")
            checksum = integer(record, "checksum")
            if name == "mmap":
                map_ns = integer(record, "mmap_elapsed_ns")
                touch_ns = integer(record, "touch_elapsed_ns")
                if record["elapsed_ns"] != map_ns + touch_ns:
                    raise ValueError("mmap duration components mismatch")
                page_size = integer(record, "page_size", 2)
                size = integer(record, "bytes")
                if (size != config["mmap_mib"] * 1048576 or size != iterations * page_size or
                        checksum != page_checksum(iterations) or
                        record.get("shuffle_seed") != "4d595df4d0f33173"):
                    raise ValueError("mmap size/content/seed mismatch")
                expected_warmup = 16
            else:
                limit = {"syscall": 10000, "pipe": 100, "fork": 10}[name]
                if iterations != config[name + "_iterations"]:
                    raise ValueError("iteration count mismatch")
                expected_warmup = min(limit, iterations)
                if name == "fork" and checksum != iterations:
                    raise ValueError("fork checksum mismatch")
                if name == "syscall" and checksum % iterations:
                    raise ValueError("getpid checksum mismatch")
                if name == "pipe":
                    # Sum the checked complement tokens, including the warm-up offset.
                    cycle = sum((i % 251) ^ 255 for i in range(251))
                    blocks, tail = divmod(iterations, 251)
                    expected = blocks * cycle + sum(((expected_warmup + i) % 251) ^ 255
                                                   for i in range(tail))
                    if checksum != expected:
                        raise ValueError("pipe checksum mismatch")
            if integer(record, "warmup_iterations") != expected_warmup:
                raise ValueError("internal warmup mismatch")
            variants.append(record["variant"])
            samples.append(dict(record, case_id=case, warmup=active["warmup"],
                                repetition=case // 4 - warmups))
        elif kind == "end":
            if (active is None or integer(record, "id", 0) != case or
                    record.get("passed") is not True or record.get("timed_out") is not False or
                    integer(record, "status", 0) != 0 or variants != VARIANTS[active["benchmark"]]):
                raise ValueError("failed child, timeout, or missing/incorrect measurement")
            active = None
            case += 1
        elif kind == "suite_end":
            if (active is not None or case != expected_cases or record.get("passed") is not True or
                    integer(record, "cases", 0) != expected_cases):
                raise ValueError("incomplete or failed suite")
            finished = True
        else:
            raise ValueError(f"unknown record type: {kind!r}")
    if not finished:
        raise ValueError("missing suite_end (timeout, crash, or premature exit)")
    return environment, samples


def parse_log(path):
    text = path.read_text(errors="replace")
    if FATAL.search(text):
        raise ValueError("fatal kernel diagnostic")
    records = []
    for line in text.splitlines():
        if line.startswith("AZBENCH "):
            records.append(json.loads(line[len("AZBENCH "):]))
    return records


def spread(values):
    median = statistics.median(values)
    quartiles = statistics.quantiles(values, n=4, method="inclusive") if len(values) > 1 else [median] * 3
    return {"n": len(values), "median": median, "min": min(values), "max": max(values),
            "iqr": quartiles[2] - quartiles[0],
            "median_absolute_deviation": statistics.median(abs(x - median) for x in values)}


def summarize(samples):
    summary = {}
    for name in NAMES:
        for variant in VARIANTS[name]:
            selected = [s for s in samples if not s["warmup"] and
                        s["benchmark"] == name and s["variant"] == variant]
            if not selected:
                raise ValueError("no measured samples")
            metrics = {"elapsed_ns": spread([s["elapsed_ns"] for s in selected])}
            if name in ("syscall", "pipe"):
                unit = "ns_per_call" if name == "syscall" else "ns_per_round_trip"
                metrics[unit] = spread([s["elapsed_ns"] / s["iterations"] for s in selected])
            else:
                unit = "forks_per_second" if name == "fork" else "logical_pages_per_second"
                metrics[unit] = spread([s["iterations"] * 1e9 / s["elapsed_ns"] for s in selected])
                if name == "mmap":
                    metrics["logical_mib_per_second"] = spread([
                        s["bytes"] / 1048576 * 1e9 / s["elapsed_ns"] for s in selected])
                    for part in ("mmap_elapsed_ns", "touch_elapsed_ns"):
                        metrics[part] = spread([s[part] for s in selected])
            if any(not math.isfinite(x) for metric in metrics.values() for x in metric.values()):
                raise ValueError("nonfinite metric")
            summary[name + "/" + variant] = metrics
    return summary


def checked_run(args, work):
    with (work / "prepare.log").open("ab") as log:
        subprocess.run(args, cwd=REPO, stdout=log, stderr=log, check=True)


def prepare_guest(work, args, config_path):
    checked_run(["make", "-j" + str(os.cpu_count() or 1), "build/kernel.elf"], work)
    root, iso = work / "root", work / "iso-root"
    for path in (root / "sbin", root / "bench", root / "etc", root / "tmp",
                 iso / "boot/limine", iso / "EFI/BOOT"):
        path.mkdir(parents=True)
    shutil.copy2(BIN / "supervisor", root / "sbin/init.elf")
    for name in NAMES:
        shutil.copy2(BIN / ("bench_" + name), root / "bench")
    shutil.copy2(config_path, root / "etc/azbench.conf")
    checked_run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096", "-d", str(root),
                 str(work / "initrd.ext2"), "24M"], work)
    shutil.copy2(REPO / "build/kernel.elf", iso / "boot/kernel.elf")
    shutil.copy2(work / "initrd.ext2", iso / "boot/initrd.ext2")
    shutil.copy2(REPO / "limine.conf", iso / "boot/limine/limine.conf")
    limine = REPO / "tools/limine"
    if not (limine / "limine-bios-cd.bin").is_file():
        limine = Path("/usr/share/limine")
    assets = {}
    for name in ("limine-bios-cd.bin", "limine-bios.sys"):
        shutil.copy2(limine / name, iso / "boot/limine")
        assets[name] = sha(limine / name)
    efi_options = []
    if args.firmware == "uefi":
        for name, location in (("BOOTX64.EFI", iso / "EFI/BOOT"),
                               ("limine-uefi-cd.bin", iso / "boot/limine")):
            shutil.copy2(limine / name, location)
            assets[name] = sha(limine / name)
        efi_options = ["-eltorito-alt-boot", "-e", "boot/limine/limine-uefi-cd.bin", "-no-emul-boot"]
    if shutil.which("xorriso"):
        mkiso = ["xorriso", "-as", "mkisofs"]
    else:
        tool = shutil.which("mkisofs") or shutil.which("genisoimage")
        if not tool:
            raise ValueError("need xorriso, mkisofs, or genisoimage")
        mkiso = [tool]
    checked_run(mkiso + ["-b", "boot/limine/limine-bios-cd.bin", "-no-emul-boot",
                         "-boot-load-size", "4", "-boot-info-table", "-R", "-J"] + efi_options +
                ["-o", str(work / "test.iso"), str(iso)], work)
    if (limine / "limine").is_file():
        checked_run([str(limine / "limine"), "bios-install", str(work / "test.iso")], work)
    firmware = []
    if args.firmware == "uefi":
        pairs = [(Path("/usr/share/OVMF/OVMF_CODE.fd"), Path("/usr/share/OVMF/OVMF_VARS.fd")),
                 (Path("/usr/share/edk2/ovmf/OVMF_CODE.fd"), Path("/usr/share/edk2/ovmf/OVMF_VARS.fd"))]
        code, variables = next(((c, v) for c, v in pairs if c.is_file() and v.is_file()), (None, None))
        if code is None:
            raise ValueError("UEFI requested but OVMF firmware unavailable")
        shutil.copy2(variables, work / "ovmf-vars.fd")
        assets.update(ovmf_code=sha(code), ovmf_vars_source=sha(variables))
        firmware = ["-drive", f"if=pflash,format=raw,readonly=on,file={code}",
                    "-drive", f"if=pflash,format=raw,file={work / 'ovmf-vars.fd'}"]
    accel = (["-enable-kvm"] if args.accelerator == "kvm" else
             ["-accel", "tcg,thread=multi,tb-size=512"])
    qemu = ["qemu-system-x86_64", "-M", "q35", "-m", args.ram, "-smp", str(args.smp),
            *accel, "-cpu", args.cpu, *firmware, "-cdrom", str(work / "test.iso"),
            "-serial", f"file:{work / 'serial.log'}", "-vga", "std", "-display", "none",
            "-nic", "none", "-no-reboot", "-no-shutdown"]
    assets.update(kernel=sha(iso / "boot/kernel.elf"), limine_conf=sha(REPO / "limine.conf"),
                  initrd=sha(work / "initrd.ext2"), iso=sha(work / "test.iso"))
    return qemu, assets


def stop(process):
    # New process session contains QEMU or the host supervisor and its children.
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def execute(argv, work, target, timeout):
    log_path = work / "serial.log"
    console = work / "qemu.log" if target == "guest" else log_path
    with console.open("wb") as output:
        process = subprocess.Popen(argv, cwd=REPO, stdout=output, stderr=output, start_new_session=True)
        deadline = time.monotonic() + timeout
        try:
            while True:
                text = log_path.read_text(errors="replace") if log_path.exists() else ""
                if FATAL.search(text):
                    raise ValueError("fatal kernel diagnostic")
                # A complete line is necessary; final validation verifies all its fields.
                complete = any(line.startswith('AZBENCH {"type":"suite_end",')
                               for line in text.splitlines(keepends=True) if line.endswith("\n"))
                if target == "guest" and complete:
                    time.sleep(0.2)
                    break
                if process.poll() is not None:
                    if target == "linux" and process.returncode == 0:
                        break
                    raise ValueError(f"{target} process exited {process.returncode} before success")
                if time.monotonic() >= deadline:
                    raise ValueError("host supervision timeout")
                time.sleep(0.2)
        finally:
            stop(process)
    return parse_log(log_path)


def positive(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("linux", "guest"), default="guest")
    parser.add_argument("--accelerator", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="bios")
    parser.add_argument("--cpu")
    parser.add_argument("--smp", type=positive, default=4)
    parser.add_argument("--ram", default="1536M")
    for option, fallback in (("repetitions", 10), ("warmups", 1), ("syscall-iterations", 10000000),
                             ("pipe-iterations", 10000), ("mmap-mib", 256), ("fork-iterations", 1000),
                             ("case-timeout", 120), ("timeout", 1800)):
        parser.add_argument("--" + option, type=positive, default=fallback)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    config = {key: getattr(args, key) for key in ("repetitions", "warmups", "syscall_iterations",
              "pipe_iterations", "mmap_mib", "fork_iterations", "case_timeout")}
    limits = (1000, 100, 1000000000, 100000000, 65536, 1000000, 86400)
    if any(value > limit for value, limit in zip(config.values(), limits)):
        parser.error("configuration exceeds a supervisor limit")
    if not re.fullmatch(r"[1-9][0-9]*[MG]", args.ram):
        parser.error("--ram must be a positive size ending in M or G")
    args.cpu = args.cpu or ("host" if args.accelerator == "kvm" else "max,+rdrand,+rdseed")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    label = "linux-host" if args.target == "linux" else args.accelerator
    work = (args.output or REPO / "build/bench/results" / (label + "-" + stamp)).resolve()
    if not work.is_relative_to((REPO / "build").resolve()):
        parser.error("--output must be a new directory under build/")
    try:
        work.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(str(error))
    manifest = {"schema_version": 1, "started_utc": stamp, "status": "preparing",
                "target": args.target, "accelerator": label, "config": config,
                "host_timeout_seconds": args.timeout, "payloads": {},
                "host_uname": command(["uname", "-a"]),
                "host_affinity": sorted(os.sched_getaffinity(0)),
                "host_load_average": list(os.getloadavg()),
                "baseline_eligible": args.repetitions >= 10}
    try:
        for name in (*("bench_" + n for n in NAMES), "supervisor"):
            info = json.loads((BIN / (name + ".json")).read_text())
            if sha(BIN / name) != info["binary_sha256"]:
                raise ValueError(f"payload changed since build: {name}")
            for source, fingerprint in info["sources"].items():
                if sha(REPO / "tests/bench" / source) != fingerprint:
                    raise ValueError(f"source changed since build: {source}")
            manifest["payloads"][name] = info
        for proc_file in ("cpuinfo", "meminfo", "version"):
            shutil.copyfile(Path("/proc") / proc_file, work / ("host-" + proc_file + ".txt"))
        config_path = work / "config.txt"
        config_path.write_text("\n".join(str(value) for value in config.values()) + "\n")
        if args.target == "guest":
            if args.accelerator == "kvm" and not os.access("/dev/kvm", os.R_OK | os.W_OK):
                manifest["status"] = "skipped"
                manifest["reason"] = "/dev/kvm unavailable"
                save(work / "manifest.json", manifest)
                print(f"SKIP: {manifest['reason']}; {work}")
                return 77
            argv, assets = prepare_guest(work, args, config_path)
            manifest.update(guest={"cpu": args.cpu, "firmware": args.firmware, "smp": args.smp,
                                   "ram": args.ram, "machine": "q35", "assets_sha256": assets},
                            qemu_version=command(["qemu-system-x86_64", "--version"]))
        else:
            argv = [str(BIN / "supervisor"), str(config_path), str(BIN)]
        manifest.update(command=argv, source=source_state(), status="running")
        save(work / "manifest.json", manifest)
        print(f"Running {args.target} ({label}), {args.repetitions} measured repetitions; {work}", flush=True)
        records = execute(argv, work, args.target, args.timeout)
        (work / "records.jsonl").write_text("".join(json.dumps(r, sort_keys=True) + "\n" for r in records))
        environment, samples = validate(records, config)
        (work / "samples.jsonl").write_text("".join(json.dumps(s, sort_keys=True) + "\n" for s in samples))
        summary = summarize(samples)
        save(work / "summary.json", summary)
        manifest.update(status="passed", environment=environment)
        print(json.dumps(summary, indent=2))
        print(f"PASS: results saved in {work}")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        manifest.update(status="failed", error=str(error))
        print(f"FAIL: {error}; evidence: {work}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        manifest.update(status="failed", error="interrupted by user")
        return 130
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save(work / "manifest.json", manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
