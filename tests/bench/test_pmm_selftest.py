#!/usr/bin/env python3
"""Corrupt a real PMM boot log to test the evidence validator."""
import argparse
import json
from pathlib import Path
import tempfile
from pmm_selftest import observe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--cpus", type=int, required=True)
    args = parser.parse_args()
    original = args.log.read_text()
    records = observe(args.log, args.cpus)
    lines = ["PMMTEST " + json.dumps(r) for r in records]

    def changed(phase, field, value):
        data = [dict(r) for r in records]
        data[phase][field] = value
        return "\n".join("PMMTEST " + json.dumps(r) for r in data) + \
            "\n== probe complete: 121 passed, 0 failed ==\n"

    corrupt = {
        "missing": "\n".join(lines[:-1]),
        "duplicate": original + lines[0] + "\n",
        "reordered": "\n".join(lines[::-1]),
        "bool_counter": changed(1, "allocations", True),
        "wrong_zeroing": changed(0, "zero_bytes", 0),
        "wrong_cache": changed(1, "cached_pages", 12),
        "wrong_ownership": changed(1, "pages", records[1]["pages"] + 1),
        "wrong_recovery": changed(2, "recovered", records[2]["seed"] + 4096),
        "wrong_restore": changed(3, "restored_pages", 0),
        "wrong_cpu": changed(4, "cpus", 99),
        "missing_abi": "\n".join(lines),
        "panic": original + "PANIC\n",
        "skip": original + 'PMMTEST {"phase":"skip"}\n',
    }
    with tempfile.TemporaryDirectory(prefix="azami-pmm-test-parser-") as folder:
        path = Path(folder) / "corrupt.txt"
        for name, text in corrupt.items():
            path.write_text(text)
            try:
                observe(path, args.cpus)
            except ValueError:
                print("PASS rejected", name)
            else:
                raise AssertionError("accepted " + name)
    print("PASS real complete log and", len(corrupt), "corrupt variants")


if __name__ == "__main__":
    main()
