#!/usr/bin/env python3
"""Compare additional I/O argument and page-fault cases with real Linux."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

from usercopy_observe import FIELDS, REPO, comparison as compare_base, execute, observe as observe_base

ARGUMENTS = ("invalid-zero", "invalid-null", "invalid-huge", "zero", "null", "bad", "huge",
             "above-user", "v-invalid-zero", "v-invalid-negative", "v-invalid-too-many", "v-zero",
             "v-negative", "v-too-many", "v-null", "v-bad", "v-zero-payload", "v-overflow", "v-max-zero")
NAMES = []
for direction in ("read", "write"):
    NAMES += [f"memfd.{direction}.args.{argument}" for argument in ARGUMENTS]
    NAMES += [f"pipe.{direction}.args.wrong-end-zero"]
for stream in ("memfd", "pipe"):
    for direction in ("read", "write"):
        NAMES += [f"{stream}.{direction}.page.{mode}.{prefix}"
                  for mode in ("none", "readonly", "unmapped") for prefix in (1, 2048, 4095)]
        NAMES += [f"{stream}.{direction}.page.valid.4096"]
for direction in ("read", "write"):
    NAMES += [f"memfd.{direction}.page.{mode}.4096" for mode in ("none", "vector-page")]


def observe(path):
    return observe_base(path, names=NAMES, mode="compat-extension", max_size=8192, max_prefix=4096)


def comparison(linux, guest):
    return compare_base(linux, guest, names=NAMES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accel", nargs="+", choices=("kvm", "tcg"), default=["tcg"])
    parser.add_argument("--kernel", type=Path, default=REPO / "build/kernel.elf")
    parser.add_argument("--payload", type=Path, default=REPO / "tools/linux/out/bin/azami-usercopy-compat")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--linux-only", action="store_true")
    parser.add_argument("--collect-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    payload = output / "probe"
    payload.write_bytes(args.payload.read_bytes()); payload.chmod(0o700)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    source = REPO / "tools/linux/tests/usercopy-compat.c"
    source_digest = sha(source)
    execute([str(payload)], output / "linux.txt", 15, dict(os.environ, UCOPY_OBSERVE_EXIT="1"))
    linux = observe(output / "linux.txt")
    guests, comparisons = {}, {}
    kernel = None
    if not args.linux_only:
        kernel = output / "kernel.elf"
        kernel.write_bytes(args.kernel.read_bytes())
        for accel in dict.fromkeys(args.accel):
            work = output / accel
            env = dict(os.environ, LINUX_TEST_KERNEL=str(kernel), LINUX_TEST_WORK=str(work),
                       LINUX_TEST_ACCEL=accel, LINUX_TEST_SMP="4", LINUX_TEST_MEM_MB="1536",
                       LINUX_TEST_NET="none", LINUX_TEST_TIMEOUT="60")
            execute([str(REPO / "scripts/linux-test.sh"), str(payload)], output / (accel + ".txt"), 90, env)
            guests[accel] = observe(work / "serial.log")
            comparisons[accel] = comparison(linux, guests[accel])
            print(f"{accel}: {len(NAMES)} observations, {len(NAMES)-len(comparisons[accel])} match Linux, "
                  f"{len(comparisons[accel])} differ", flush=True)
    metadata = dict(mode="compat-extension", cases=len(NAMES), payload_sha256=sha(payload),
                    probe_source_sha256=source_digest, linux_release=os.uname().release,
                    fields=list(FIELDS), comparison=comparisons,
                    compatible=bool(guests) and all(not differences for differences in comparisons.values()))
    if kernel:
        metadata.update(kernel_sha256=sha(kernel), machine="q35", cpus=4, memory_mb=1536)
    rows = [dict(name=name, linux=[linux[i][field] for field in FIELDS],
                 **{accel: [records[i][field] for field in FIELDS] for accel, records in guests.items()})
            for i, name in enumerate(NAMES)]
    if sha(source) != source_digest:
        raise ValueError("probe source changed during observation")
    encoded = json.dumps(metadata, indent=2)[:-2] + ',\n  "observations": [\n'
    encoded += ",\n".join("    " + json.dumps(row, separators=(",", ":")) for row in rows)
    (output / "summary.json").write_text(encoded + "\n  ]\n}\n")
    if args.linux_only:
        print(f"Linux oracle: {len(NAMES)} observations collected; no guest comparison", flush=True)
    return 0 if args.linux_only or args.collect_only or metadata["compatible"] else 1


if __name__ == "__main__":
    sys.exit(main())
