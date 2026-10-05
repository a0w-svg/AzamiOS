#!/usr/bin/env python3
# =============================================================================
# AzamiOS — Syscall Table Consistency Checker
# File: scripts/check_syscall_tbl.py
#
# Validates that kernel/syscall/syscalls.tbl is strictly consistent with
# include/azami/uapi/syscall_nr.h.
# =============================================================================
import re
import sys
import os

TBL_PATH = "kernel/syscall/syscalls.tbl"
NR_PATH = "include/azami/uapi/syscall_nr.h"

def main():
    if not os.path.exists(TBL_PATH):
        sys.stderr.write(f"Missing {TBL_PATH}\n")
        return 1
    if not os.path.exists(NR_PATH):
        sys.stderr.write(f"Missing {NR_PATH}\n")
        return 1

    nr_map = {}
    with open(NR_PATH, "r") as f:
        for line in f:
            m = re.match(r"#define\s+(SYS_\w+)\s+(\d+)", line)
            if m:
                nr_map[m.group(1)] = int(m.group(2))

    errors = 0
    tbl_entries = []
    seen_nrs = set()
    prev_nr = -1

    with open(TBL_PATH, "r") as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 4:
                sys.stderr.write(f"{TBL_PATH}:{line_num}: invalid format\n")
                errors += 1
                continue

            nr = int(parts[0])
            abi = parts[1]
            name = parts[2]
            fn = parts[3]

            if nr in seen_nrs:
                sys.stderr.write(f"{TBL_PATH}:{line_num}: duplicate nr {nr}\n")
                errors += 1
            if nr <= prev_nr:
                sys.stderr.write(f"{TBL_PATH}:{line_num}: nr {nr} is not monotonically increasing (prev was {prev_nr})\n")
                errors += 1
            prev_nr = nr
            seen_nrs.add(nr)

            macro = "SYS_" + name
            if macro not in nr_map:
                sys.stderr.write(f"{TBL_PATH}:{line_num}: {macro} not found in {NR_PATH}\n")
                errors += 1
            elif nr_map[macro] != nr:
                sys.stderr.write(f"{TBL_PATH}:{line_num}: {macro} has number {nr} in .tbl, but {nr_map[macro]} in {NR_PATH}\n")
                errors += 1

            tbl_entries.append((nr, abi, name, fn))

    if errors > 0:
        print(f"FAILED: {errors} inconsistencies found between {TBL_PATH} and {NR_PATH}")
        return 1

    print(f"PASS: {len(tbl_entries)} syscalls in {TBL_PATH} verified against {NR_PATH} with 0 errors.")
    return 0

if __name__ == "__main__":
    sys.exit(main())

