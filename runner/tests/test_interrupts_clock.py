"""Exercise the storm's production 32-bit clock checks without an emulator."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "work" / "interrupts-clock-test"


class Clock(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32)
                for name in ("start", "previous", "sample", "elapsed")] + [
                    ("invalid", ctypes.c_int)]


@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class StormClock(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        BUILD.mkdir(parents=True, exist_ok=True)
        cls.addClassCleanup(shutil.rmtree, BUILD)
        source = BUILD / "clock.c"
        source.write_text(
            '#include <stdint.h>\n'
            'typedef uint32_t ULONG;\n'
            'typedef int32_t LONG;\n'
            '#include "interrupts_clock.h"\n'
            'void init(struct ir_clock *c, ULONG n) { ir_clock_init(c, n); }\n'
            'int sample(struct ir_clock *c, ULONG n) { return ir_clock_sample(c, n); }\n')
        env = dict(os.environ, TMPDIR=str(BUILD))
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC", "-I", str(ROOT / "disk" / "modules"),
                        str(source), "-o", str(BUILD / "clock.so")],
                       check=True, env=env, capture_output=True)
        cls.lib = ctypes.CDLL(str(BUILD / "clock.so"))
        cls.lib.init.argtypes = [ctypes.POINTER(Clock), ctypes.c_uint32]
        cls.lib.sample.argtypes = [ctypes.POINTER(Clock), ctypes.c_uint32]
        cls.lib.sample.restype = ctypes.c_int

    def clock(self, start):
        c = Clock()
        self.lib.init(ctypes.byref(c), start)
        return c

    def sample(self, c, now):
        return self.lib.sample(ctypes.byref(c), now)

    def test_equal_samples_are_valid(self):
        c = self.clock(123)
        self.assertEqual(self.sample(c, 123), 1)
        self.assertEqual(c.elapsed, 0)
        self.assertEqual(c.invalid, 0)

    def test_normal_full_storm(self):
        c = self.clock(123)
        for elapsed in (1, 142, 3000, 709379, 5 * 709379):
            self.assertEqual(self.sample(c, 123 + elapsed), 1)
            self.assertEqual(c.elapsed, elapsed)

    def test_first_sample_rebound_cannot_expire_storm(self):
        c = self.clock(123)
        self.assertEqual(self.sample(c, 122), 0)
        self.assertEqual(c.invalid, 1)
        self.assertEqual(c.elapsed, 0)
        self.assertEqual((c.start, c.previous, c.sample), (123, 123, 122))

    def test_rebound_during_soft_wait_is_rejected(self):
        c = self.clock(123)
        self.assertEqual(self.sample(c, 1000), 1)
        self.assertEqual(self.sample(c, 999), 0)
        self.assertEqual(c.elapsed, 877)

    def test_actual_counter_wrap_is_valid(self):
        c = self.clock(0xfffffff0)
        for now, elapsed in ((0xffffffff, 15), (0, 16), (0x1234, 0x1244)):
            self.assertEqual(self.sample(c, now), 1)
            self.assertEqual(c.elapsed, elapsed)
        self.assertEqual(c.invalid, 0)

    def test_backward_across_zero_is_not_a_forward_wrap(self):
        c = self.clock(0)
        self.assertEqual(self.sample(c, 0xffffffff), 0)
        self.assertEqual(c.invalid, 1)

    def test_offending_sample_survives_cleanup_read(self):
        c = self.clock(123)
        self.assertEqual(self.sample(c, 122), 0)
        self.assertEqual(self.sample(c, 130), 0)
        self.assertEqual((c.start, c.previous, c.sample), (123, 123, 122))


if __name__ == "__main__":
    unittest.main()
