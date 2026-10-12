#!/usr/bin/env python3
"""Collect user-copy effects and compare them with a real Linux oracle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests/semantics"))
from namespaces import execute

FIELDS = ("return", "errno", "offset", "remaining_or_size", "data_checksum", "copied_prefix", "canary")
MODES = ("vector-data", "vector-desc", "zero-scalar", "zero-vector", "null-data", "valid")
NAMES = []
for stream in ("memfd", "pipe"):
    for direction in ("read", "write"):
        NAMES += [f"{stream}.{direction}.{mode}.{prefix}"
                  for mode in ("none", "readonly", "unmapped") for prefix in (1, 16, 63)]
        NAMES += [f"{stream}.{direction}.{mode}.0" for mode in MODES]


def observe(path, *, names=NAMES, mode="observation", max_size=128, max_prefix=63):
    text = Path(path).read_text(errors="replace")
    if re.search(r"PANIC|Triple fault|Kernel panic|UCOPYFAIL|divergence", text):
        raise ValueError("fatal diagnostic or incomplete observation")
    environments, records, sequence = [], [], []
    for line in text.splitlines():
        if line.startswith("UCOPYENV "):
            environments.append(json.loads(line[9:])); sequence.append("environment")
        elif line.startswith("UCOPYCASE "):
            records.append(json.loads(line[10:])); sequence.append("case")
        elif line.startswith("== usercopy observations complete:"):
            sequence.append("complete")
        elif line.startswith("== probe complete:"):
            sequence.append("verdict")
    cases = len(names)
    if environments != [dict(mode=mode, page_bytes=4096, cases=cases)]:
        raise ValueError("unexpected observation environment")
    if any(type(environments[0][key]) is not int for key in ("page_bytes", "cases")):
        raise ValueError("invalid environment counter types")
    if sequence != ["environment"] + ["case"] * cases + ["complete", "verdict"]:
        raise ValueError("missing, repeated or reordered observations/verdict")
    if (re.findall(r"== usercopy observations complete: .* ==", text) !=
            [f"== usercopy observations complete: {cases} collected =="] or
            re.findall(r"== probe complete: .* ==", text) != [f"== probe complete: {cases} passed, 0 failed =="]):
        raise ValueError("incomplete collection verdict")
    if [r.get("name") if isinstance(r, dict) else None for r in records] != names:
        raise ValueError("missing or inconsistent case identity")
    for record in records:
        if set(record) != {"name", *FIELDS} or type(record["canary"]) is not bool:
            raise ValueError("invalid record schema")
        for field in FIELDS[:-1]:
            if type(record[field]) is not int or not -1 <= record[field] < 2**63:
                raise ValueError("invalid numeric observation")
        if ((record["return"] == -1 and not 0 < record["errno"] < 4096) or
                (record["return"] >= 0 and record["errno"] != 0)):
            raise ValueError("inconsistent errno observation")
        if (not 0 <= record["remaining_or_size"] <= max_size or record["data_checksum"] < 0 or
                not 0 <= record["copied_prefix"] <= max_prefix):
            raise ValueError("out-of-range data observation")
        if record["name"].startswith("pipe.") and record["offset"] != -1:
            raise ValueError("unexpected pipe offset representation")
        if record["name"].startswith("memfd.") and record["offset"] < 0:
            raise ValueError("failed file-position observation")
    return records


def comparison(linux, guest, *, names=NAMES):
    if (len(linux) != len(names) or len(guest) != len(names) or
            [r["name"] for r in linux] != names or [r["name"] for r in guest] != names):
        raise ValueError("incomplete or reordered oracle comparison")
    return [dict(name=reference["name"], fields=[f for f in FIELDS if reference[f] != actual[f]])
            for reference, actual in zip(linux, guest) if reference != actual]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accel", nargs="+", choices=("kvm", "tcg"), default=["tcg"])
    parser.add_argument("--kernel", type=Path, default=REPO / "build/kernel.elf")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--collect-only", action="store_true",
                        help="return collection success even when Linux comparisons differ")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    payload = REPO / "tools/linux/out/bin/azami-usercopy-observe"
    kernel = output / "kernel.elf"
    kernel.write_bytes(args.kernel.read_bytes())
    immutable_payload = output / "probe"
    immutable_payload.write_bytes(payload.read_bytes()); immutable_payload.chmod(0o700)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    source = REPO / "tools/linux/tests/usercopy-observe.c"
    source_digest = sha(source)
    env = dict(os.environ, UCOPY_OBSERVE_EXIT="1")
    execute([str(immutable_payload)], output / "linux.txt", 15, env)
    linux = observe(output / "linux.txt")
    guests, comparisons = {}, {}
    for accel in dict.fromkeys(args.accel):
        work = output / accel
        env = dict(os.environ, LINUX_TEST_KERNEL=str(kernel), LINUX_TEST_WORK=str(work),
                   LINUX_TEST_ACCEL=accel, LINUX_TEST_SMP="4", LINUX_TEST_MEM_MB="1536",
                   LINUX_TEST_NET="none", LINUX_TEST_TIMEOUT="60")
        execute([str(REPO / "scripts/linux-test.sh"), str(immutable_payload)],
                output / (accel + ".txt"), 90, env)
        guests[accel] = observe(work / "serial.log")
        comparisons[accel] = comparison(linux, guests[accel])
        print(f"{accel}: 60 observations, {60-len(comparisons[accel])} match Linux, "
              f"{len(comparisons[accel])} differ", flush=True)
    metadata = dict(mode="observation", cases=60, kernel_sha256=sha(kernel),
                    payload_sha256=sha(immutable_payload),
                    probe_source_sha256=source_digest,
                    linux_release=os.uname().release, machine="q35", cpus=4, memory_mb=1536,
                    fields=list(FIELDS), comparison=comparisons,
                    compatible=all(not differences for differences in comparisons.values()))
    rows = [dict(name=name, linux=[linux[i][f] for f in FIELDS],
                 **{a: [records[i][f] for f in FIELDS] for a, records in guests.items()})
            for i, name in enumerate(NAMES)]
    if sha(source) != source_digest:
        raise ValueError("probe source changed during observation")
    # Keep each complete observation on one reviewable line.
    encoded = json.dumps(metadata, indent=2)[:-2] + ',\n  "observations": [\n'
    encoded += ",\n".join("    " + json.dumps(row, separators=(",", ":")) for row in rows)
    (output / "summary.json").write_text(encoded + "\n  ]\n}\n")
    return 0 if args.collect_only or metadata["compatible"] else 1


if __name__ == "__main__":
    sys.exit(main())
