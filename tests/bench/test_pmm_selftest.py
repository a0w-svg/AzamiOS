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
    parser.add_argument("--drop-cpu", type=int, choices=(2,))
    parser.add_argument("--worker-probe", action="store_true")
    args = parser.parse_args()
    original = args.log.read_text()
    records = observe(args.log, args.cpus, args.drop_cpu, args.worker_probe)

    def rendered(data):
        rows = [("PMMTEST " if "phase" in r else "PMMWORKER ") + json.dumps(r)
                for r in data]
        if args.worker_probe:
            rows.append("== PMM worker probe complete: 0 failed ==")
        rows.append(f"== probe complete: {8 if args.worker_probe else 121} passed, 0 failed ==")
        return "\n".join(rows) + "\n"

    def changed(phase, field, value):
        data = [dict(r) for r in records]
        data[phase][field] = value
        return rendered(data)

    corrupt = {
        "missing": rendered([r for i, r in enumerate(records) if i != 4]),
        "duplicate": original + "PMMTEST " + json.dumps(records[0]) + "\n",
        "reordered": rendered(records[:5][::-1] + records[5:]),
        "bool_counter": changed(1, "allocations", True),
        "wrong_zeroing": changed(0, "zero_bytes", 0),
        "wrong_cache": changed(1, "cached_pages", 12),
        "wrong_ownership": changed(1, "pages", records[1]["pages"] + 1),
        "wrong_recovery": changed(2, "recovered", records[2]["seed"] + 4096),
        "missing_pressure": changed(2, "pressure_zones", 0),
        "wrong_dma32_pressure": changed(2, "dma32_pressure_zones", 3),
        "missing_irq_failure": changed(2, "irq_disabled_failure", False),
        "missing_sweep": changed(2, "sweeps", 0),
        "wrong_reclaimed": changed(2, "reclaimed_pages", records[2]["reclaimed_pages"] + 1),
        "wrong_callbacks": changed(2, "callbacks", records[2]["callbacks"] + 1),
        "wrong_timeouts": changed(2, "rpc_timeouts", records[2]["rpc_timeouts"] + 1),
        "lock_contention": changed(2, "busy", 1),
        "wrong_cpu_budget": changed(2, "cpu_budget", 9),
        "wrong_page_budget": changed(2, "page_budget", 257),
        "wrong_callback_budget": changed(2, "callback_budget", 33),
        "wrong_restore": changed(3, "restored_pages", 0),
        "wrong_cpu": changed(4, "cpus", 99),
        "missing_payload_verdict": "\n".join(line for line in original.splitlines()
                                            if not line.startswith("== probe complete:")),
        "contradictory_payload_verdict": original + "== probe complete: 0 passed, 1 failed ==\n",
        "panic": original + "PANIC\n",
        "skip": original + 'PMMTEST {"phase":"skip"}\n',
    }
    if args.worker_probe:
        corrupt.update({
            "missing_worker": rendered(records[:5]),
            "duplicate_worker": original + "PMMWORKER " + json.dumps(records[5]) + "\n",
            "reordered_worker": rendered(records[5:] + records[:5]),
            "bool_worker_started": changed(5, "worker_started", True),
            "worker_not_started": changed(5, "worker_started", 0),
            "worker_not_scheduled": changed(5, "worker_cycles", 1),
            "worker_no_sweep": changed(5, "worker_sweeps", 0),
            "worker_unbounded_epoch": changed(5, "worker_sweeps", 9),
            "worker_quiescence_too_early": changed(5, "quiescent_cycles", 9),
            "worker_not_observed_after_quiescence": changed(5, "worker_cycles", records[5]["quiescent_cycles"] + 1),
            "worker_resumed_after_quiescence": changed(5, "quiescent_sweeps", records[5]["worker_sweeps"] + 1),
            "worker_pressure_remains": changed(5, "pending_zones", 1),
            "only_boot_sweep": changed(5, "sweeps", 1),
            "only_boot_callbacks": changed(5, "callbacks", records[2]["callbacks"]),
            "lost_timeout_accounting": changed(5, "rpc_timeouts", records[2]["rpc_timeouts"] - 1),
            "worker_start_failure": changed(5, "start_failures", 1),
            "worker_clock_unavailable": changed(5, "time_unavailable", 1),
            "worker_invalid_zone": changed(5, "pending_zones", 4),
            "missing_worker_verdict": original.replace("== PMM worker probe complete: 0 failed ==", ""),
            "contradictory_worker_verdict": original + "== PMM worker probe complete: 1 failed ==\n",
        })
        missing_field = [dict(r) for r in records]
        del missing_field[5]["reclaimed_pages"]
        corrupt["missing_worker_counter"] = rendered(missing_field)
    with tempfile.TemporaryDirectory(prefix="azami-pmm-test-parser-") as folder:
        path = Path(folder) / "corrupt.txt"
        for name, text in corrupt.items():
            path.write_text(text)
            try:
                observe(path, args.cpus, args.drop_cpu, args.worker_probe)
            except ValueError:
                print("PASS rejected", name)
            else:
                raise AssertionError("accepted " + name)
    print("PASS real complete log and", len(corrupt), "corrupt variants")


if __name__ == "__main__":
    main()
