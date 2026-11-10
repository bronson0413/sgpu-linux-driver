"""Unit tests for fuzzlog.py. Inputs are small hand-written samples of the log formats."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import fuzzlog  # noqa: E402

STATS = [
    "2026/11/09 19:24:53 machine check:\n",
    "2026/11/09 19:25:00 candidates=0 corpus=10 coverage=500 exec total=0 (0/sec) pending=0 reproducing=0\n",
    "2026/11/09 19:26:00 candidates=0 corpus=50 coverage=950 exec total=6000 (100/sec) pending=0 reproducing=0\n",
    "2026/11/09 19:35:00 candidates=0 corpus=60 coverage=1000 exec total=60000 (100/sec) pending=0 reproducing=0\n",
]


class FuzzTests(unittest.TestCase):
    def test_summary(self):
        s = fuzzlog.parse_fuzz(STATS)
        self.assertEqual((s.samples, s.duration_s, s.executions), (3, 600, 60000))
        self.assertEqual((s.final_corpus, s.final_coverage), (60, 1000))
        self.assertEqual(s.average_exec_per_s, 100.0)
        self.assertEqual(s.time_to_90pct_coverage_s, 60)

    def test_slow_rate_units(self):
        lines = [
            "2026/11/10 02:21:49 candidates=28 corpus=0 coverage=0 exec total=50 (284/hour) pending=0\n",
            "2026/11/10 02:22:49 candidates=28 corpus=3 coverage=9 exec total=170 (2/min) pending=0\n",
        ]
        s = fuzzlog.parse_fuzz(lines)
        self.assertEqual((s.samples, s.executions, s.final_corpus), (2, 170, 3))

    def test_no_statistics(self):
        self.assertIsNone(fuzzlog.parse_fuzz(["serving http\n"]))

    def test_zero_duration(self):
        self.assertEqual(fuzzlog.parse_fuzz(STATS[1:2]).average_exec_per_s, 0.0)


class ReportTests(unittest.TestCase):
    def test_classification(self):
        lines = [
            "[   12.1] BUG: KCSAN: data-race in foo / bar\n",
            "[   13.2] UBSAN: shift-out-of-bounds in drivers/x.c:10:2\n",
            "WARNING: possible circular locking dependency detected\n",
            "[   14.0] WARNING: CPU: 1 PID: 22 at kernel/foo.c:3 foo+0x1/0x2\n",
            "ordinary line\n",
        ]
        kinds = [r.kind for r in fuzzlog.parse_reports(lines)]
        self.assertEqual(kinds, ["kcsan", "ubsan", "lockdep", "warning"])

    def test_clean_log(self):
        self.assertEqual(fuzzlog.parse_reports(["sgpu ready, 16 contexts\n"]), [])


class KcsanTests(unittest.TestCase):
    def test_split(self):
        lines = [
            "[ 1.0] BUG: KCSAN: data-race in a / b\n",
            "[ 1.1] read to 0xff of 8 bytes by task 3 on cpu 0:\n",
            "[ 1.2]  sgpu_ioctl+0x10/0x20 [sgpu]\n",
            "[ 2.0] BUG: KCSAN: data-race in c / d\n",
            "[ 2.1] read to 0xff of 8 bytes by task 3 on cpu 0:\n",
            "[ 2.2]  vfs_read+0x10/0x20\n",
        ]
        ours, other = fuzzlog.split_kcsan(lines)
        self.assertEqual(len(ours), 1)
        self.assertIn("a / b", ours[0])
        self.assertEqual(len(other), 1)

    def test_driver_only_as_caller_is_not_a_driver_race(self):
        lines = [
            "[ 1.0] BUG: KCSAN: data-race in osq_lock / osq_unlock\n",
            "[ 1.1] write (marked) to 0xff of 8 bytes by task 3 on cpu 0:\n",
            "[ 1.2]  osq_lock+0xbd/0x250\n",
            "[ 1.3]  mutex_lock_interruptible+0x94/0xa0\n",
            "[ 1.4]  mychar_ioctl+0x1c9/0x380 [mychar]\n",
        ]
        ours, other = fuzzlog.split_kcsan(lines)
        self.assertEqual((ours, len(other)), ([], 1))


if __name__ == "__main__":
    unittest.main()
