#!/usr/bin/env python3
"""Compare the namespace probe's Linux and AzamiOS observations."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
PAYLOAD = REPO / "tools/linux/out/bin/azami-namespace-probe"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")


def command(args):
    return subprocess.check_output(args, cwd=REPO, text=True).strip()


def source_state():
    files = subprocess.check_output(["git", "ls-files", "-z", "--cached", "--others",
                                     "--exclude-standard"], cwd=REPO)
    entries = {}
    for encoded in sorted(set(files.split(b"\0")) - {b""}):
        name = os.fsdecode(encoded)
        path = REPO / name
        if path.is_symlink():
            content = os.fsencode(os.readlink(path))
        elif path.is_file():
            content = path.read_bytes()
        else:
            content = b"<deleted>"
        entries[name] = hashlib.sha256(content).hexdigest()
    return {"revision": command(["git", "rev-parse", "HEAD"]),
            "status": command(["git", "status", "--short", "--untracked-files=all"]),
            "content_sha256": hashlib.sha256(json.dumps(entries, sort_keys=True).encode()).hexdigest(),
            "files_sha256": entries}


def execute(args, output, timeout, env=None):
    with output.open("wb") as log:
        process = subprocess.Popen(args, cwd=REPO, stdout=log, stderr=log,
                                   env=env, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
            if code:
                raise ValueError(f"command failed with exit {code}: {args[0]}")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def parse(path):
    text = path.read_text(errors="replace")
    if re.search(r"PANIC|Triple fault|Kernel panic|free list/bitmap divergence|orders_mask/free_list divergence", text):
        raise ValueError("fatal kernel diagnostic")
    cases, skips = {}, []
    for line in text.splitlines():
        if line.startswith("NSCASE "):
            record = json.loads(line[7:])
            if (not isinstance(record, dict) or not isinstance(record.get("name"), str) or
                    not record["name"] or record["name"] in cases or record.get("passed") is not True or
                    type(record.get("value")) is not int or type(record.get("expected")) is not int or
                    record["value"] != record["expected"]):
                raise ValueError(f"duplicate or failed case: {record}")
            cases[record["name"]] = record
        elif line.startswith("NSSKIP "):
            record = json.loads(line[7:])
            if (not isinstance(record, dict) or not isinstance(record.get("name"), str) or
                    not isinstance(record.get("reason"), str) or not record["name"] or
                    not record["reason"] or any(item["name"] == record["name"] for item in skips)):
                raise ValueError("invalid or duplicate skip")
            skips.append(record)
    verdicts = re.findall(r"== probe complete: ([0-9]+) passed, ([0-9]+) failed ==", text)
    if verdicts != [(str(len(cases)), "0")] or not cases:
        raise ValueError("missing/inconsistent verdict or measurements")
    skipped = re.findall(r"== namespaces skipped: ([0-9]+) ==", text)
    if skipped != [str(len(skips))]:
        raise ValueError("inconsistent skip count")
    return {"passed": len(cases), "cases": cases, "skips": skips}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--linux-mode", choices=("auto", "userns", "native"), default="auto")
    parser.add_argument("--suite", choices=("namespaces", "openat2", "exec", "epoll", "rename", "signals", "perf", "fd-lifetime", "sysfs-pagination"), default="namespaces")
    parser.add_argument("--smp", type=int, default=4, help="guest vCPUs (1-64)")
    args = parser.parse_args()
    payload = PAYLOAD if args.suite == "namespaces" else REPO / f"tools/linux/out/bin/azami-{args.suite}-probe"
    if args.timeout < 1:
        parser.error("--timeout must be positive")
    if not 1 <= args.smp <= 64:
        parser.error("--smp must be between 1 and 64")
    parent = REPO / "build/semantics"
    parent.mkdir(parents=True, exist_ok=True)
    if args.output:
        work = args.output.resolve()
        if not work.is_relative_to((REPO / "build").resolve()):
            parser.error("--output must be a new directory under build/")
        work.mkdir(parents=True, exist_ok=False)
    else:
        work = Path(tempfile.mkdtemp(prefix=args.suite + "-", dir=parent))
    manifest = {"started_utc": datetime.now(timezone.utc).isoformat(), "status": "running",
                "source": source_state(), "kernel_sha256": digest(REPO / "build/kernel.elf"),
                "payload_sha256": digest(payload), "suite": args.suite, "timeout_seconds": args.timeout,
                "compiler": command([str(REPO / "tools/linux/musl/bin/musl-gcc"), "--version"]),
                "flags": "-static -std=c11 -O2 -g -Wall -Wextra -Werror -pthread",
                "qemu": command(["qemu-system-x86_64", "--version"]),
                "guest": {"firmware": "bios", "machine": "q35", "smp": args.smp, "ram": "1536M",
                          "accelerator": "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg",
                          "cmdline": os.environ.get("LINUX_TEST_CMDLINE", "")},
                "host_uname": command(["uname", "-a"])}
    save(work / "manifest.json", manifest)
    print(f"{args.suite} semantic comparison: {work}", flush=True)
    try:
        linux = [str(payload)]
        unshare = shutil.which("unshare")
        if args.suite == "namespaces" and args.linux_mode != "native" and unshare:
            setup = subprocess.run([unshare, "-Ur", "true"], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, timeout=10)
            manifest["linux_userns_setup"] = {"exit": setup.returncode,
                                              "stderr": setup.stderr.decode(errors="replace")}
            if setup.returncode == 0:
                linux = [unshare, "-Ur", str(payload)]
            elif args.linux_mode == "userns":
                raise ValueError("requested Linux user namespace unavailable")
        elif args.suite == "namespaces" and args.linux_mode == "userns":
            raise ValueError("unshare utility unavailable")
        manifest["linux_command"] = linux
        execute(linux, work / "linux.txt", args.timeout)
        host = parse(work / "linux.txt")
        env = dict(os.environ, LINUX_TEST_WORK=str(work / "vm"), LINUX_TEST_TIMEOUT=str(args.timeout),
                   LINUX_TEST_SMP=str(args.smp), LINUX_TEST_KERNEL=str(REPO / "build/kernel.elf"))
        if args.suite == "exec":
            root = work / "root"
            (root / "test-exec").mkdir(parents=True)
            (root / "lib").mkdir()
            libc = REPO / "tools/linux/out/bin/libc.so"
            dynamic = REPO / "tools/linux/out/bin/azami-exec-dynamic"
            shutil.copy2(libc, root / "test-exec/libc.so")
            shutil.copy2(dynamic, root / "test-exec/azami-exec-dynamic")
            # Keep a valid alternative libc at an old fallback location: the
            # missing embedded interpreter must still return ENOENT.
            shutil.copy2(libc, root / "lib/ld-musl-x86_64.so.1")
            manifest["exec_fixtures_sha256"] = {"libc.so": digest(libc),
                                               "dynamic": digest(dynamic)}
            manifest["dynamic_flags"] = "-std=c11 -O2 -g -Wall -Wextra -Werror -fPIE -pie -Wl,-dynamic-linker,./libc.so"
            env["LINUX_TEST_ROOT"] = str(root)
        guest_command = [str(REPO / "scripts/linux-test.sh"), str(payload)]
        manifest["guest_command"] = guest_command
        execute(guest_command, work / "guest.txt", args.timeout + 30, env)
        guest = parse(work / "guest.txt")
        required = {"namespaces": "unshare.unsupported_20000000", "openat2": "race.fd_close_reuse",
                    "exec": "staged.segment_short_read", "epoll": "pwait2.pending_interrupt.restored",
                    "rename": "race.concurrent_rename", "signals": "signals.basic", "perf": "race.fd.lifetime",
                    "fd-lifetime": "fd.final_owner.in_flight_observed",
                    "sysfs-pagination": "sysfs.cpu.enumeration.exact"}.get(args.suite)
        allowed_skips = ({"hardware.instructions"} if args.suite == "perf" else
                         {"sysfs.pmu.cpu"} if args.suite == "sysfs-pagination" else set())
        if any(item["name"] not in allowed_skips for item in guest["skips"]) or (required and required not in guest["cases"]):
            raise ValueError("guest coverage missing or skipped")
        if args.suite == "fd-lifetime" and (guest["passed"] != 21 or host["passed"] != 21):
            raise ValueError("complete FD lifetime coverage required")
        if args.suite == "sysfs-pagination":
            if guest["passed"] != (51 if guest["skips"] else 60):
                raise ValueError("complete sysfs pagination coverage required")
            counts = [json.loads(line[9:]) for line in (work / "vm/serial.log").read_text().splitlines()
                      if line.startswith("SYSFSOBS ") and "present_cpu_count" in line]
            if counts != [{"present_cpu_count": args.smp, "enumerated_cpu_count": args.smp}]:
                raise ValueError("requested guest CPU enumeration incomplete")
            required_sysfs = {"sysfs.cpu.cookie.seek", "sysfs.cpu.rewind", "sysfs.cpu.metadata.lookup",
                              "sysfs.class.cookie.seek", "sysfs.block.pagination", "sysfs.devices.pagination",
                              "sysfs.cpu.topology.direct.lookup", "sysfs.pmu.software.type"}
            if not required_sysfs <= guest["cases"].keys():
                raise ValueError("sysfs continuation/lookup coverage missing")
            manifest["sysfs_cpu_counts"] = counts[0]
        if args.suite == "perf":
            scope_cases = {"other.thread.exit.frozen", "scope.task.wrong.cpu.zero",
                           "scope.task.filtered.time", "scope.system.idle.time",
                           "race.remote.fd.lifetime", "azami.credentials.no.caps",
                           "azami.credentials.ptrace.only", "azami.credentials.perfmon.only",
                           "azami.dumpable.fork.exec.credentials", "azami.counting.identifier.guest",
                           "azami.identifier.sampling", "azami.identifier.other.sample",
                           "azami.exclude.host"}
            if not guest["skips"]:
                scope_cases |= {"hw.remote.caller.affinity.independent", "hw.scope.wrong.cpu.zero",
                                "hw.scope.system.live", "race.hw.remote.migration.control",
                                "race.hw.remote.exit.control", "hw.remote.exit.frozen",
                                "azami.hw.filters.user.only", "azami.hw.filters.raw.alias",
                                "azami.hw.filters.kernel.only", "azami.hw.filters.disabled.freeze"}
            if not scope_cases <= guest["cases"].keys():
                raise ValueError("guest perf scope coverage missing")
        common = sorted(host["cases"].keys() & guest["cases"].keys())
        if not common or any(host["cases"][name]["value"] != guest["cases"][name]["value"] for name in common):
            raise ValueError("Linux/AzamiOS observation mismatch")
        summary = {"linux": host, "azami": guest, "matching_common_cases": common,
                   "guest_only_cases": sorted(guest["cases"].keys() - host["cases"].keys())}
        save(work / "comparison.json", summary)
        manifest.update(status="passed", linux_passed=host["passed"], guest_passed=guest["passed"],
                        common_matched=len(common), linux_skips=host["skips"], guest_skips=guest["skips"],
                        iso_sha256=digest(work / "vm/test.iso"))
        print(f"PASS: guest {guest['passed']}, Linux {host['passed']}; {len(common)} matching common observations")
        if host["skips"]:
            print("Linux exclusions: " + "; ".join(item["reason"] for item in host["skips"]))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        manifest.update(status="failed", error=str(error))
        print(f"FAIL: {error}; logs: {work}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        manifest.update(status="failed", error="interrupted")
        return 130
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save(work / "manifest.json", manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
