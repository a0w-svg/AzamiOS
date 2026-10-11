#!/usr/bin/env python3
"""Reject corrupted variants of a real, complete allocator measurement log."""
import argparse
import json
from pathlib import Path
import tempfile
from pmm_run import observe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("valid_log", type=Path)
    args = parser.parse_args()
    original = args.valid_log.read_text().splitlines()
    assert len(observe(args.valid_log, True)["workers"]) == 55

    def mutate(tag, change):
        lines = original[:]
        index = next(i for i, line in enumerate(lines) if line.startswith(tag + " "))
        record = json.loads(lines[index][len(tag) + 1:])
        change(record)
        lines[index] = tag + " " + json.dumps(record)
        return lines

    corrupt = {
        "empty_environment": mutate("PMMENV", lambda r: r.clear()),
        "wrong_timer": mutate("PMMENV", lambda r: r.update(timer_resolution_ns=0)),
        "duplicate_cpus": mutate("PMMENV", lambda r: r.update(cpus=[0, 0, 2, 3])),
        "wrong_checksum": mutate("PMMBENCH", lambda r: r.update(checksum=0)),
        "zero_timing": mutate("PMMBENCH", lambda r: r.update(mmap_ns=0)),
        "wrong_cpu": mutate("PMMBENCH", lambda r: r.update(cpu=999)),
        "bad_estimate": mutate("PMMSNAPSHOT", lambda r: r.update(estimate=1)),
        "boolean_worker": mutate("PMMBENCH", lambda r: r.update(worker=False)),
        "missing_verdict": original[:-1],
    }
    reordered = original[:]
    a, b = [i for i, line in enumerate(reordered) if line.startswith("PMMSNAPSHOT ")][:2]
    reordered[a], reordered[b] = reordered[b], reordered[a]
    corrupt["reordered_barriers"] = reordered
    with tempfile.TemporaryDirectory(prefix="azami-pmm-parser-") as folder:
        path = Path(folder) / "corrupt.log"
        for name, lines in corrupt.items():
            path.write_text("\n".join(lines) + "\n")
            try:
                observe(path, True)
            except (ValueError, KeyError, TypeError):
                print("PASS rejected", name)
            else:
                raise AssertionError("accepted " + name)
    print("PASS valid55workers66snapshots and10corrupt fixtures")


if __name__ == "__main__":
    main()
