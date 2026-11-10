"""amitorture times: host seconds per disk and module from serial arrival times."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import records, times  # noqa: E402


def rec(seq, **kw):
    return records.encode({"v": 1, "seq": seq, **kw})


class Times(unittest.TestCase):
    def disk(self, lines_at):
        """lines_at: [(host s, record bytes)] -> a raw/<entry>/<disk>/ dir."""
        d = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, d)
        disk = os.path.join(d, "raw", "a500-68000-ce", "1-core")
        os.makedirs(disk)
        data, arr = b"", []
        for t, line in lines_at:
            data += line
            arr.append([t, len(data)])
        open(os.path.join(disk, "serial.bin"), "wb").write(data)
        json.dump(arr, open(os.path.join(disk, "serial-times.json"), "w"))
        return d, disk

    def test_modules_and_first_record(self):
        d, disk = self.disk([
            (10.0, rec(0, t="hello")),
            (11.0, rec(1, t="mod", mod="cia", ev="begin")),
            (15.5, rec(2, t="mod", mod="cia", ev="end")),
            (16.0, rec(3, t="mod", mod="display", ev="begin")),
            (40.0, rec(4, t="case", mod="display", case="x", st="pass")),
        ])
        t = times.disk_times(disk)
        self.assertEqual(t["first_record_s"], 10.0)
        self.assertEqual(t["last_record_s"], 40.0)
        self.assertEqual(t["modules"]["cia"], 4.5)
        self.assertEqual(t["modules"]["display"], 24.0)   # no end record: up to the last record
        self.assertEqual(list(times.run_times(d, ["a500*"])), ["a500-68000-ce"])
        self.assertEqual(times.run_times(d, ["a1200*"]), {})
        self.assertIn("display 24 s, cia", times.format_times(times.run_times(d)))

    def test_no_serial(self):
        d = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, d)
        self.assertIsNone(times.disk_times(d))


if __name__ == "__main__":
    unittest.main()
