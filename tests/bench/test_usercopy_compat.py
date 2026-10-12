#!/usr/bin/env python3
"""Validate the extended oracle parser against a real collection and corruptions."""
import argparse
import json
from pathlib import Path
import tempfile
from usercopy_compat import NAMES, comparison, observe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    original = args.log.read_text()
    records = observe(args.log)
    assert len(records) == 84 and not comparison(records, records)
    changed = [dict(record) for record in records]
    changed[0]["offset"] += 1
    assert comparison(records, changed) == [dict(name=NAMES[0], fields=["offset"])]
    for incomplete in (records[:-1], records[::-1], []):
        try:
            comparison(records, incomplete)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted incomplete comparison")

    def rendered(rows):
        return 'UCOPYENV {"mode":"compat-extension","page_bytes":4096,"cases":84}\n' + \
            "\n".join("UCOPYCASE " + json.dumps(row) for row in rows) + \
            "\n== usercopy observations complete: 84 collected ==\n" + \
            "== probe complete: 84 passed, 0 failed ==\n"

    def modified(field, value):
        rows = [dict(record) for record in records]
        rows[0][field] = value
        return rendered(rows)

    corrupt = {
        "missing": rendered(records[:-1]),
        "duplicate": rendered(records + records[:1]),
        "reordered": rendered(records[::-1]),
        "unknown_name": modified("name", "unknown"),
        "boolean_return": modified("return", True),
        "float_environment": original.replace('"cases":84', '"cases":84.0'),
        "wrong_profile": original.replace("compat-extension", "observation"),
        "missing_field": rendered([{key: value for key, value in records[0].items() if key != "errno"}] + records[1:]),
        "extra_field": modified("unexpected", 1),
        "bad_errno": modified("errno", 0 if records[0]["return"] < 0 else 14),
        "too_large_size": modified("remaining_or_size", 8193),
        "too_large_prefix": modified("copied_prefix", 4097),
        "negative_prefix": modified("copied_prefix", -1),
        "numeric_canary": modified("canary", 1),
        "negative_file_offset": modified("offset", -1),
        "panic": original + "PANIC\n",
        "harness_failure": original + "UCOPYFAIL setup\n",
        "missing_verdict": original.replace("== probe complete: 84 passed, 0 failed ==", ""),
        "duplicate_verdict": original + "== probe complete: 84 passed, 0 failed ==\n",
        "failed_verdict": original.replace("84 passed, 0 failed", "83 passed, 1 failed"),
    }
    with tempfile.TemporaryDirectory(prefix="azami-usercopy-compat-parser-") as directory:
        path = Path(directory) / "corrupt.txt"
        for name, text in corrupt.items():
            path.write_text(text)
            try:
                observe(path)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted " + name)
    print("PASS real 84-case collection, exact comparisons and", len(corrupt), "corrupt variants")


if __name__ == "__main__":
    main()
