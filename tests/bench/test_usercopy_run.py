#!/usr/bin/env python3
"""Check real copy measurements, warmup exclusion and malformed evidence rejection."""
import argparse
import json
from pathlib import Path
import statistics
import tempfile
from usercopy_run import ORDER, SIZES, checksum, observe, save_summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    original = args.log.read_text()
    observed = observe(args.log)
    rows, environment = observed["rows"], observed["environment"]
    assert len(rows) == 66
    for size in SIZES:
        values = [row["elapsed_ns"] / row["iterations"] for row in rows
                  if row["size_bytes"] == size and row["repetition"] >= 0]
        summary = observed["summary"][str(size)]
        assert summary["samples"] == 10 and summary["ns_per_pair"]["median"] == statistics.median(values)
    assert all(checksum(size) == sum((i * 17 + size) % 251 for i in range(size)) for size in SIZES)

    def rendered(new_rows=rows, env=environment):
        return "COPYENV " + json.dumps(env) + "\n" + \
            "\n".join("COPYBENCH " + json.dumps(row) for row in new_rows) + \
            "\n== probe complete: 66 passed, 0 failed ==\n"

    def modified(field, value):
        new_rows = [dict(row) for row in rows]
        new_rows[0][field] = value
        return rendered(new_rows)

    def env_modified(field, value):
        return rendered(env={**environment, field: value})

    missing_field = [{key: value for key, value in rows[0].items() if key != "elapsed_ns"}] + rows[1:]
    corrupt = {
        "missing_case": rendered(rows[:-1]),
        "duplicate_case": rendered(rows + rows[:1]),
        "reordered": rendered(rows[::-1]),
        "wrong_size": modified("size_bytes", 17),
        "wrong_repetition": modified("repetition", 0),
        "wrong_iterations": modified("iterations", rows[0]["iterations"] - 1),
        "wrong_checksum": modified("checksum", rows[0]["checksum"] + 1),
        "boolean_elapsed": modified("elapsed_ns", True),
        "zero_elapsed": modified("elapsed_ns", 0),
        "missing_field": rendered(missing_field),
        "extra_field": modified("unexpected", 0),
        "false_canary": modified("canary", False),
        "numeric_canary": modified("canary", 1),
        "negative_cpu": env_modified("cpu", -1),
        "boolean_cpu": env_modified("cpu", True),
        "zero_resolution": env_modified("timer_resolution_ns", 0),
        "float_cases": env_modified("cases", 66.0),
        "wrong_warmups": env_modified("warmups", 0),
        "duplicate_environment": "COPYENV " + json.dumps(environment) + "\n" + rendered(),
        "duplicate_json_field": rendered().replace('"cpu":', '"cpu":0,"cpu":', 1),
        "malformed_prefix": rendered().replace("COPYBENCH ", "COPYBENCH:", 1),
        "non_object": rendered().replace("COPYBENCH " + json.dumps(rows[0]), "COPYBENCH []", 1),
        "fatal": rendered() + "COPYFAIL integrity\n",
        "panic": rendered() + "PANIC\n",
        "missing_verdict": rendered().replace("== probe complete: 66 passed, 0 failed ==", ""),
        "failed_verdict": rendered().replace("66 passed, 0 failed", "65 passed, 1 failed"),
        "duplicate_verdict": rendered() + "== probe complete: 66 passed, 0 failed ==\n",
        "early_verdict": "== probe complete: 66 passed, 0 failed ==\n" + rendered(),
    }
    with tempfile.TemporaryDirectory(prefix="azami-copy-bench-parser-") as directory:
        path = Path(directory) / "evidence.txt"
        for name, text in corrupt.items():
            path.write_text(text)
            try:
                observe(path)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted " + name)
        # An arbitrarily slow warmup must not change any measured summary.
        altered = [dict(row) for row in rows]
        for row in altered[:len(SIZES)]:
            row["elapsed_ns"] = 2**62
        path.write_text(rendered(altered))
        assert observe(path)["summary"] == observed["summary"]
        save_summary(path, {"linux": observed})
        assert json.loads(path.read_text()) == {"linux": observed}
    assert [(row["repetition"], row["size_bytes"]) for row in rows] == ORDER
    print("PASS real 66-case copy workload, warmup exclusion, compact JSON and", len(corrupt), "corrupt variants")


if __name__ == "__main__":
    main()
