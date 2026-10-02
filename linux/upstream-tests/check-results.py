#!/usr/bin/env python3
"""Check that upstream output and interrupted runs cannot become false passes."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("upstream_run", Path(__file__).with_name("run.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Results(unittest.TestCase):
    def test_console_status_between_shell_header_and_result(self):
        case = {"name": "hush/hush-heredoc"}
        for result, expected in [("ok", "PASS"), ("fail (1)", "FAIL")]:
            output = "-n hush-heredoc/huge.tests:\nC33 display: blanked while idle\n " + result + "\n"
            self.assertEqual(runner.verdict("busybox", case, 0, output)["status"], expected)
            output = "hush-heredoc/huge.tests:C33 display: blanked while idle\n " + result + "\n"
            self.assertEqual(runner.verdict("busybox", case, 0, output)["status"], expected)

    def test_shell_failure_overrides_success_status(self):
        case = {"name": "hush/hush-bugs"}
        self.assertEqual(runner.verdict("busybox", case, 0, "-n hush-bugs/example.tests:\n fail (1)\n")["status"], "FAIL")

    def test_empty_success_is_unexecuted(self):
        self.assertEqual(runner.verdict("busybox", {"name": "applets/missing"}, 0, "")["status"], "UNTESTED")

    def test_timeout_keeps_completed_assertions(self):
        got = runner.verdict("busybox", {"name": "applets/xargs"}, 124, "PASS: one\n")
        self.assertEqual(got, {"status": "TIMEOUT", "assertions": {"PASS": 1}})

    def test_expected_nonzero_is_upstream_decision(self):
        self.assertEqual(runner.verdict("uclibc", {"name": "example"}, 0, "PASS example got 1 expected 1\n")["status"], "PASS")

    def test_crash_requires_entered_test(self):
        self.assertTrue(runner.crashed("UPSTREAM-BEGIN one\nstop reason: runaway", "one"))
        self.assertFalse(runner.crashed("UPSTREAM-BEGIN one\nstop reason: runaway", "two"))
        self.assertFalse(runner.crashed("UPSTREAM-BEGIN one-more\nstop reason: runaway", "one"))
        self.assertFalse(runner.crashed("UPSTREAM-BEGIN one\nstop reason: powered off", "one"))


if __name__ == "__main__":
    unittest.main()
