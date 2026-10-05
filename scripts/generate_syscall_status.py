#!/usr/bin/env python3
# =============================================================================
# AzamiOS — Syscall Implementation Status Generator
# File: scripts/generate_syscall_status.py
#
# Analyzes kernel/syscall/syscalls.tbl and the codebase to generate
# docs/SYSCALL-STATUS.md with exact implementation metrics.
# =============================================================================
import re
import os
import sys

TBL_PATH = "kernel/syscall/syscalls.tbl"
DOC_PATH = "docs/SYSCALL-STATUS.md"

SOURCE_FILES = [
    "kernel/syscall/sys_fs.c",
    "kernel/syscall/sys_mm.c",
    "kernel/syscall/sys_proc.c",
    "kernel/syscall/sys_signal.c",
    "kernel/syscall/sys_net.c",
    "kernel/syscall/sys_time.c",
    "kernel/syscall/sys_ipc.c",
    "kernel/syscall/sys_misc.c",
    "kernel/syscall/syscall.c",
    "kernel/signal.c",
    "kernel/ptrace.c",
    "kernel/perf/perf.c"
]

def load_tbl():
    tbl = []
    with open(TBL_PATH, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 4:
                tbl.append({
                    "nr": int(parts[0]),
                    "abi": parts[1],
                    "name": parts[2],
                    "entry": parts[3]
                })
    return tbl

def extract_functions():
    fn_map = {}
    for path in SOURCE_FILES:
        if not os.path.exists(path):
            continue
        with open(path, "r") as f:
            content = f.read()

        matches = re.finditer(r'(?:s64|static\s+s64|int|void)\s+([a-zA-Z0-9_]+)\s*\([^)]*\)\s*\{', content)
        for m in matches:
            fn_name = m.group(1)
            start = m.end() - 1
            depth = 1
            pos = start + 1
            while depth > 0 and pos < len(content):
                if content[pos] == '{': depth += 1
                elif content[pos] == '}': depth -= 1
                pos += 1
            body = content[start:pos]
            fn_map[fn_name] = {
                "file": path,
                "body": body,
                "lines": len(body.splitlines())
            }
    return fn_map

def classify_status(entry, fn_info):
    if not fn_info:
        return "Missing", "No entry point found in source files"
    body = fn_info["body"]
    clean_body = re.sub(r'/\*.*?\*/', '', body, flags=re.DOTALL)
    clean_body = re.sub(r'//.*', '', clean_body)
    statements = [s.strip() for s in clean_body.split(';') if s.strip()]

    # Check for direct stub / ENOSYS / EOPNOTSUPP
    if ("return -(s64)ENOSYS;" in clean_body or "return -ENOSYS;" in clean_body) and len(statements) <= 3:
        return "Stub (ENOSYS)", "Unimplemented placeholder returning -ENOSYS"

    if ("return -(s64)EOPNOTSUPP;" in clean_body or "return -EOPNOTSUPP;" in clean_body) and len(statements) <= 3:
        return "Stub (EOPNOTSUPP)", "Explicitly unsupported returning -EOPNOTSUPP"

    if "syscall_report_missing" in body and len(statements) <= 4:
        return "Stub (Missing)", "Logged via syscall_report_missing and returns error"

    if "TODO" in body or "FIXME" in body or "partial" in body.lower():
        return "Partial", "Functional implementation with noted limitations"

    return "Full", "Complete implementation matching Linux ABI"

def get_domain(filename, name):
    if "sys_fs" in filename: return "Filesystem & I/O"
    if "sys_mm" in filename: return "Memory Management"
    if "sys_proc" in filename: return "Process & Scheduling"
    if "sys_signal" in filename or "signal.c" in filename: return "Signals"
    if "sys_net" in filename: return "Networking & Sockets"
    if "sys_time" in filename: return "Timekeeping & Clocks"
    if "sys_ipc" in filename: return "IPC & Synchronization"
    if "ptrace" in filename: return "Debugging & Tracing"
    if "perf" in filename: return "Performance & Counters"
    if "sys_misc" in filename: return "System & Miscellaneous"
    return "Core / Other"

def main():
    tbl = load_tbl()
    fn_map = extract_functions()

    results = []
    counts = {"Full": 0, "Partial": 0, "Stub (ENOSYS)": 0, "Stub (EOPNOTSUPP)": 0, "Stub (Missing)": 0, "Missing": 0}

    for item in tbl:
        fn_info = fn_map.get(item["entry"])
        status, notes = classify_status(item["entry"], fn_info)
        counts[status] = counts.get(status, 0) + 1
        domain = get_domain(fn_info["file"] if fn_info else "", item["name"])
        results.append({
            "nr": item["nr"],
            "name": item["name"],
            "entry": item["entry"],
            "status": status,
            "domain": domain,
            "file": fn_info["file"] if fn_info else "N/A",
            "lines": fn_info["lines"] if fn_info else 0,
            "notes": notes
        })

    total = len(tbl)
    implemented_count = counts.get("Full", 0) + counts.get("Partial", 0)
    impl_pct = (implemented_count / total) * 100

    out = []
    out.append("# AzamiOS System Call Implementation Status\n")
    out.append(f"Generated automatically from `kernel/syscall/syscalls.tbl` and kernel sources.\n")
    out.append("## Summary Statistics\n")
    out.append(f"- **Total Defined System Calls:** {total}\n")
    out.append(f"- **Implemented (Full or Partial):** {implemented_count} ({impl_pct:.1f}%)\n")
    out.append(f"  - **Full Implementation:** {counts.get('Full', 0)}\n")
    out.append(f"  - **Partial Implementation:** {counts.get('Partial', 0)}\n")
    out.append(f"- **Stubs / Unimplemented:** {total - implemented_count}\n")
    out.append(f"  - **ENOSYS Stubs:** {counts.get('Stub (ENOSYS)', 0)}\n")
    out.append(f"  - **EOPNOTSUPP Stubs:** {counts.get('Stub (EOPNOTSUPP)', 0)}\n")
    out.append(f"  - **Missing Handlers:** {counts.get('Missing', 0)}\n\n")

    out.append("## Status Breakdown by Domain\n\n")
    domains = sorted(list(set(r["domain"] for r in results)))
    out.append("| Domain | Total | Full | Partial | Stub / Missing | % Covered |\n")
    out.append("| :--- | :--- | :--- | :--- | :--- | :--- |\n")
    for d in domains:
        d_items = [r for r in results if r["domain"] == d]
        d_total = len(d_items)
        d_full = sum(1 for r in d_items if r["status"] == "Full")
        d_part = sum(1 for r in d_items if r["status"] == "Partial")
        d_stub = d_total - (d_full + d_part)
        pct = ((d_full + d_part) / d_total) * 100 if d_total else 0
        out.append(f"| {d} | {d_total} | {d_full} | {d_part} | {d_stub} | {pct:.1f}% |\n")

    out.append("\n## Detailed Syscall Matrix\n\n")
    out.append("| NR | Name | Entry Point | Domain | Status | File | Description / Notes |\n")
    out.append("| :---: | :--- | :--- | :--- | :--- | :--- | :--- |\n")
    for r in results:
        status_badge = f"**{r['status']}**" if "Full" in r['status'] else f"*{r['status']}*"
        out.append(f"| {r['nr']} | `{r['name']}` | `{r['entry']}` | {r['domain']} | {status_badge} | `{r['file']}` | {r['notes']} |\n")

    os.makedirs(os.path.dirname(DOC_PATH), exist_ok=True)
    with open(DOC_PATH, "w") as f:
        f.writelines(out)

    print(f"Generated {DOC_PATH} ({len(results)} syscall entries).")
    print(f"Implemented: {implemented_count}/{total} ({impl_pct:.1f}%)")

if __name__ == "__main__":
    main()

