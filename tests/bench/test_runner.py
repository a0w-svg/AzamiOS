#!/usr/bin/env python3
"""Runner failure tests using real child processes and the emitted protocol."""
import copy
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import run


class RunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base = run.REPO / "build/bench/runner-tests"
        base.mkdir(parents=True, exist_ok=True)
        cls.work = Path(tempfile.mkdtemp(prefix="run-", dir=base))
        cls.config = dict(repetitions=1, warmups=1, syscall_iterations=10000,
                          pipe_iterations=257, mmap_mib=1, fork_iterations=3, case_timeout=1)
        cls.config_path = cls.work / "config.txt"
        cls.config_path.write_text("\n".join(str(v) for v in cls.config.values()) + "\n")
        good = cls.work / "good"
        good.mkdir()
        cls.records = run.execute([str(run.BIN / "supervisor"), str(cls.config_path), str(run.BIN)],
                                  good, "linux", 10)

    def altered(self, kind, field=None, value=None, remove=False):
        records = copy.deepcopy(self.records)
        index = next(i for i, r in enumerate(records) if r["type"] == kind)
        if remove:
            del records[index]
        else:
            records[index][field] = value
        return records

    def test_all_workloads_and_statistics(self):
        environment, samples = run.validate(self.records, self.config)
        self.assertEqual(environment["sysname"], "Linux")
        self.assertEqual(len(samples), 10)
        self.assertEqual(len(run.summarize(samples)), 5)

    def test_missing_measurement(self):
        with self.assertRaises(ValueError):
            run.validate(self.altered("sample", remove=True), self.config)

    def test_missing_final_verdict(self):
        with self.assertRaises(ValueError):
            run.validate(self.records[:-1], self.config)

    def test_invalid_durations(self):
        for value in (0, -1, 1.5, True, None, 2**64):
            with self.subTest(value=value), self.assertRaises(ValueError):
                run.validate(self.altered("sample", "elapsed_ns", value), self.config)

    def test_wrong_counts_checksum_or_warmup(self):
        for field, value in (("iterations", 1), ("checksum", 1), ("warmup_iterations", 1)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                run.validate(self.altered("sample", field, value), self.config)

    def test_failed_or_timed_out_child(self):
        for field, value in (("status", 256), ("passed", False), ("timed_out", True)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                run.validate(self.altered("end", field, value), self.config)

    def test_duplicate_and_out_of_order_samples(self):
        records = copy.deepcopy(self.records)
        index = next(i for i, r in enumerate(records) if r["type"] == "sample")
        records.insert(index, records[index])
        with self.assertRaises(ValueError):
            run.validate(records, self.config)
        with self.assertRaises(ValueError):
            run.validate(self.altered("begin", "id", 1), self.config)

    def fixture(self, label, replacement):
        directory = self.work / label
        directory.mkdir()
        for name in run.NAMES:
            (directory / ("bench_" + name)).symlink_to(run.BIN / ("bench_" + name))
        target = directory / "bench_syscall"
        target.unlink()
        target.symlink_to(replacement)
        return directory

    def test_real_child_exits_without_measurements(self):
        directory = self.fixture("missing", "/bin/true")
        records = run.execute([str(run.BIN / "supervisor"), str(self.config_path), str(directory)],
                              directory, "linux", 10)
        with self.assertRaises(ValueError):
            run.validate(records, self.config)

    def test_real_child_failure(self):
        directory = self.fixture("failure", "/bin/false")
        with self.assertRaises(ValueError):
            run.execute([str(run.BIN / "supervisor"), str(self.config_path), str(directory)],
                        directory, "linux", 10)
        records = run.parse_log(directory / "serial.log")
        self.assertFalse(records[-1]["passed"])
        self.assertEqual(records[-2]["status"], 256)

    def test_guest_supervisor_timeout(self):
        # sleep receives the syscall count (10000 seconds); the supervisor must
        # kill and reap it at the one-second per-child deadline.
        directory = self.fixture("child-timeout", "/bin/sleep")
        with self.assertRaises(ValueError):
            run.execute([str(run.BIN / "supervisor"), str(self.config_path), str(directory)],
                        directory, "linux", 10)
        records = run.parse_log(directory / "serial.log")
        self.assertTrue(records[-2]["timed_out"])
        self.assertFalse(records[-1]["passed"])

    def test_host_timeout_and_premature_exit(self):
        for label, argv in (("host-timeout", ["/bin/sleep", "10"]),
                            ("premature", ["/bin/true"])):
            directory = self.work / label
            directory.mkdir()
            with self.subTest(label=label), self.assertRaises(ValueError):
                records = run.execute(argv, directory, "linux", 0.1)
                run.validate(records, self.config)

    def test_host_timeout_reaps_supervised_child(self):
        directory = self.fixture("supervised-host-timeout", "/bin/sleep")
        with self.assertRaises(ValueError):
            run.execute([str(run.BIN / "supervisor"), str(self.config_path), str(directory)],
                        directory, "linux", 0.3)
        records = run.parse_log(directory / "serial.log")
        end = next(r for r in records if r["type"] == "end")
        with self.assertRaises(ProcessLookupError):
            os.kill(end["child_pid"], 0)

    def test_fatal_and_malformed_output(self):
        path = self.work / "corrupt.log"
        for value in ("PANIC\n", "AZBENCH {invalid}\n"):
            path.write_text(value)
            with self.assertRaises(ValueError):
                run.parse_log(path)

    def test_invalid_program_arguments(self):
        for name in run.NAMES:
            for argument in ("0", "-1", "1x", "18446744073709551616"):
                with self.subTest(name=name, argument=argument):
                    result = subprocess.run([str(run.BIN / ("bench_" + name)), argument],
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn(b"AZBENCH ", result.stdout)

    def test_malformed_config(self):
        for index, text in enumerate(("", "1\n" * 6, "0\n" * 7, "1\n" * 8)):
            path = self.work / f"bad-config-{index}.txt"
            path.write_text(text)
            result = subprocess.run([str(run.BIN / "supervisor"), str(path), str(run.BIN)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
