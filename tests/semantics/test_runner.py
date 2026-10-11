#!/usr/bin/env python3
"""Ensure corrupt/missing namespace evidence cannot produce a pass."""
import json
from pathlib import Path
import tempfile
import unittest
import namespaces


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "log.txt"
        self.record = {"name": "case", "value": 1, "expected": 1, "passed": True}

    def protocol(self, record=None):
        return "NSCASE " + json.dumps(record or self.record) + "\n" + \
            "== namespaces skipped: 0 ==\n== probe complete: 1 passed, 0 failed ==\n"

    def parse(self, text):
        self.path.write_text(text)
        return namespaces.parse(self.path)

    def test_valid(self):
        self.assertEqual(self.parse(self.protocol())["passed"], 1)

    def test_missing_verdict(self):
        with self.assertRaises(ValueError):
            self.parse(self.protocol().replace("== probe complete: 1 passed, 0 failed ==", ""))

    def test_duplicate_case(self):
        with self.assertRaises(ValueError):
            self.parse("NSCASE " + json.dumps(self.record) + "\n" + self.protocol())

    def test_failed_case(self):
        with self.assertRaises(ValueError):
            self.parse(self.protocol(dict(self.record, passed=False)))

    def test_mismatched_observation(self):
        with self.assertRaises(ValueError):
            self.parse(self.protocol(dict(self.record, value=2)))

    def test_bool_is_not_integer(self):
        with self.assertRaises(ValueError):
            self.parse(self.protocol(dict(self.record, value=True)))

    def test_inconsistent_count(self):
        with self.assertRaises(ValueError):
            self.parse(self.protocol().replace("1 passed", "2 passed"))

    def test_skip_count(self):
        with self.assertRaises(ValueError):
            self.parse(self.protocol().replace("skipped: 0", "skipped: 1"))

    def test_malformed_record(self):
        with self.assertRaises(ValueError):
            self.parse(self.protocol().replace(json.dumps(self.record), "[]"))

    def test_duplicate_skip(self):
        skip = 'NSSKIP {"name":"excluded","reason":"unavailable"}\n'
        with self.assertRaises(ValueError):
            self.parse(skip + skip + self.protocol())

    def test_panic_after_verdict(self):
        with self.assertRaises(ValueError):
            self.parse(self.protocol() + "PANIC: failure\n")


if __name__ == "__main__":
    unittest.main()
