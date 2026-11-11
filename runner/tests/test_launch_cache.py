"""Launch overhead: a machine's config is checked once per run, a (disk,
params) is baked once per run, and the cached copies are the same."""
import os
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import adf, amiberry_cfg  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORE = os.path.join(ROOT, "disk", "build", "set", "amitorture-1-core.adf")


class VerifyKey(unittest.TestCase):
    def cfg(self, d, **kw):
        lines = {"cpu_model": "68020", "floppy0": f"{d}/media/amt-df0.adf", "serial_port": "TCP://127.0.0.1:23541",
                 "warp": "true", "floppy_speed": "0", "hardfile2": f"rw,DH0:{d}/media/hd.hdf,32,1,2,512,0,,uae"}
        lines.update(kw)
        return "\n".join(f"{k}={v}" for k, v in lines.items())

    def test_per_launch_lines_do_not_count(self):
        a = amiberry_cfg.verify_key("/bin/true", self.cfg("/w/e001"), "/w/e001")
        b = amiberry_cfg.verify_key("/bin/true", self.cfg("/w/e002", serial_port="TCP://127.0.0.1:23599",
                                                          warp="false", floppy_speed="100"), "/w/e002")
        self.assertEqual(a, b)

    def test_the_machine_counts(self):
        a = amiberry_cfg.verify_key("/bin/true", self.cfg("/w/e001"), "/w/e001")
        self.assertNotEqual(a, amiberry_cfg.verify_key("/bin/true", self.cfg("/w/e001", cpu_model="68030"), "/w/e001"))
        self.assertNotEqual(a, amiberry_cfg.verify_key("/bin/sh", self.cfg("/w/e001"), "/w/e001"))


@unittest.skipUnless(os.path.isfile(CORE), "build the disk set first (make -C disk set)")
class BakeCache(unittest.TestCase):
    def test_cached_bake_is_the_same_disk(self):
        d = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, d)
        params = {"modules": "0x100", "seed": "3"}
        fresh = adf.bake(CORE, os.path.join(d, "fresh.adf"), params)
        lock = threading.Lock()
        first = adf.bake(CORE, os.path.join(d, "a.adf"), params, cache_dir=os.path.join(d, "c"), lock=lock)
        second = adf.bake(CORE, os.path.join(d, "b.adf"), params, cache_dir=os.path.join(d, "c"), lock=lock)
        self.assertEqual(len(os.listdir(os.path.join(d, "c"))), 1)
        for x in ("a.adf", "b.adf"):
            with open(os.path.join(d, x), "rb") as fa, open(os.path.join(d, "fresh.adf"), "rb") as fb:
                self.assertEqual(fa.read(), fb.read(), x)
        self.assertEqual(first["params"], fresh["params"])
        self.assertFalse(first["baked_from_cache"])
        self.assertTrue(second["baked_from_cache"])
        other = adf.bake(CORE, os.path.join(d, "c2.adf"), {"modules": "0x80"}, cache_dir=os.path.join(d, "c"),
                         lock=lock)
        self.assertNotEqual(other["params"], fresh["params"])
        self.assertEqual(len(os.listdir(os.path.join(d, "c"))), 2)


if __name__ == "__main__":
    unittest.main()


class FloppySpeed(unittest.TestCase):
    def test_modules_are_told_the_drive_speed(self):
        from amitorture import runner as R
        self.assertEqual(R.module_floppy_speed({"options": {}}, R.FLOPPY_TEST_SPEED), "800")
        self.assertEqual(R.module_floppy_speed({"options": {}}, "0"), "turbo")
        self.assertEqual(R.module_floppy_speed({"options": {"floppy_speed": "0"}}, None), "turbo")
        self.assertEqual(R.module_floppy_speed({"options": {}}, None), "100")
        self.assertEqual(R.module_floppy_speed({"options": {}}, "800", {"floppy_speed": "100"}), "100")  # narrowing

    def test_turbo_load_restores_the_disk_speed(self):
        from amitorture import runner as R

        class IPC:
            def request(self, line, timeout=15):
                self.line = line
                return "OK"
        res = {"floppy_speed": "800", "warp": {"boot": True}}
        self.assertEqual(R.turbo_load(IPC(), res, {"options": {}}), 800)
        self.assertIsNone(R.turbo_load(IPC(), dict(res, floppy_speed="0"), {"options": {}}))
        self.assertIsNone(R.turbo_load(IPC(), res, {"options": {"floppy_speed": "0"}}))
        self.assertIsNone(R.turbo_load(IPC(), dict(res, warp={"boot": False}), {"options": {}}))

    @unittest.skipUnless(os.path.isfile(CORE), "build the disk set first (make -C disk set)")
    def test_floppy_param_is_baked(self):
        d = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, d)
        for v, want in (("800", 800), ("turbo", 0xFFFF), ("100", 100)):
            info = adf.bake(CORE, os.path.join(d, f"{v}.adf"), {"floppy": v})
            self.assertEqual(info["params"]["floppy"], want)
