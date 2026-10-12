#!/usr/bin/env python3
"""Corrupt a real fault-observation log and check oracle comparison behavior."""
import argparse
import json
from pathlib import Path
import tempfile
from usercopy_observe import comparison, observe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    original = args.log.read_text()
    records = observe(args.log)
    assert not comparison(records, records)
    changed = [dict(r) for r in records]
    changed[0]["offset"] += 1
    assert comparison(records, changed) == [dict(name=records[0]["name"], fields=["offset"])]
    for incomplete in (records[:-1], records[::-1], []):
        try:
            comparison(records, incomplete)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted incomplete oracle comparison")

    def rendered(rows):
        return 'UCOPYENV {"mode":"observation","page_bytes":4096,"cases":60}\n' + \
            "\n".join("UCOPYCASE " + json.dumps(row) for row in rows) + \
            "\n== usercopy observations complete: 60 collected ==\n" + \
            "== probe complete: 60 passed, 0 failed ==\n"

    def modified(field, value):
        rows = [dict(r) for r in records]
        rows[0][field] = value
        return rendered(rows)

    corrupt = {
        "missing_case": rendered(records[:-1]),
        "duplicate_case": rendered(records + records[:1]),
        "reordered_cases": rendered(records[::-1]),
        "wrong_name": modified("name", "unknown"),
        "bool_counter": modified("return", True),
        "float_environment": original.replace('"page_bytes":4096', '"page_bytes":4096.0'),
        "missing_field": rendered([{k:v for k,v in records[0].items() if k != "errno"}] + records[1:]),
        "extra_field": modified("unexpected", 1),
        "bad_errno": modified("errno", 0 if records[0]["return"] < 0 else 14),
        "bad_data_count": modified("remaining_or_size", 129),
        "negative_prefix": modified("copied_prefix", -1),
        "numeric_canary": modified("canary", 1),
        "panic": original + "PANIC\n",
        "harness_failure": original + "UCOPYFAIL setup\n",
        "missing_verdict": original.replace("== probe complete: 60 passed, 0 failed ==", ""),
        "repeated_verdict": original + "== probe complete: 60 passed, 0 failed ==\n",
        "failed_verdict": original.replace("60 passed, 0 failed", "59 passed, 1 failed"),
    }
    with tempfile.TemporaryDirectory(prefix="azami-usercopy-parser-") as directory:
        path = Path(directory) / "corrupt.txt"
        for name, text in corrupt.items():
            path.write_text(text)
            try:
                observe(path)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted " + name)
    print("PASS real 60-case collection, exact oracle comparison and",len(corrupt),"corrupt variants")


if __name__ == "__main__":
    main()
