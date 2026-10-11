#!/usr/bin/env python3
"""Drop CPU-1 RPC delivery in a disposable guest; require bounded errors/reuse."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
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
        work = Path(tempfile.mkdtemp(prefix="perf-timeout-", dir=parent))
    payload = REPO / "tools/linux/out/bin/azami-perf-probe"
    manifest = {"started_utc": datetime.now(timezone.utc).isoformat(), "status": "running",
                "source": source_state(), "kernel_sha256": digest(REPO / "build/kernel.elf"),
                "payload_sha256": digest(payload), "suite": "perf-timeout", "timeout_seconds": 60,
                "guest": {"firmware": "bios", "machine": "q35", "smp": 4, "ram": "1536M",
                          "accelerator": "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg",
                          "cmdline": "smp_rpc_drop_cpu=1"},
                "qemu": command(["qemu-system-x86_64", "--version"])}
    save(work / "manifest.json", manifest)
    print(f"perf RPC timeout: {work}", flush=True)
    try:
        guest_command = [str(REPO / "scripts/linux-test.sh"), str(payload)]
        manifest["guest_command"] = guest_command
        env = dict(os.environ, LINUX_TEST_WORK=str(work / "vm"), LINUX_TEST_TIMEOUT="60",
                   LINUX_TEST_CMDLINE="smp_rpc_drop_cpu=1")
        execute(guest_command, work / "guest.txt", 90, env)
        observed = parse(work / "guest.txt")
        required = {"timeout.caller.cpu", "timeout.disabled.open"}
        required |= {f"timeout.{op}.{i}" for op in ("enable", "read", "reset", "disable", "bounded") for i in range(2)}
        required |= {f"timeout.{op}.{i}" for op in ("enabled.open.rollback", "local.alive") for i in range(70)}
        if observed["skips"] or not required <= observed["cases"].keys():
            raise ValueError("timeout coverage missing or skipped")
        save(work / "observations.json", observed)
        manifest.update(status="passed", guest_passed=observed["passed"],
                        iso_sha256=digest(work / "vm/test.iso"))
        print(f"PASS: {observed['passed']} timeout/rollback/local progress observations")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        manifest.update(status="failed", error=str(error))
        print(f"FAIL: {error}; logs: {work}", file=sys.stderr)
        return 1
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save(work / "manifest.json", manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
