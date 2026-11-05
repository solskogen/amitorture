"""Instruction-policy declarations and payload flags, without guest execution."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "runner"))
sys.path.insert(0, str(ROOT / "tools"))

import mkadf
from amitorture import cli, runner


class InstructionPolicy(unittest.TestCase):
    def flags(self, options=None, params=None, entry_flags=(), extra_options=None):
        entry = {"options": options or {}, "flags": entry_flags}
        result = runner._with_entry_flags(dict(params or {}), entry, extra_options)
        payload = mkadf.default_params()
        mkadf.apply_param(payload, "flags=" + str(result["flags"]))
        return payload["flags"]

    def test_default_runner_policy_is_compatibility_but_raw_media_is_strict(self):
        self.assertEqual(self.flags(), 32 | 64)
        self.assertEqual(mkadf.default_params()["flags"], 0)
        self.assertEqual(mkadf.unpack_params(mkadf.pack_params(mkadf.default_params()))["flags"], 0)

    def test_strict_options_clear_stale_named_and_numeric_flags(self):
        options = {"cpu_no_unimplemented": "true", "fpu_no_unimplemented": "true"}
        for value in (96, "0x60", "intcompat,fpucompat", "intcompat+fpucompat+intcompat"):
            with self.subTest(value=value):
                self.assertEqual(self.flags(options, {"flags": value}), 0)
        self.assertEqual(self.flags(options, {"flags": 0x80000000 | 1 | 16 | 96}), 0x80000000 | 1 | 16)
        self.assertEqual(self.flags(options, {"flags": "unsafe,intcompat,noscreen,fpucompat"}), 1 | 16)
        self.assertEqual(self.flags(options, entry_flags=("unsafe", "intcompat", "fpucompat")), 1)

    def test_mixed_integer_and_fpu_policies_are_independent(self):
        self.assertEqual(self.flags({"cpu_no_unimplemented": "true", "fpu_no_unimplemented": "false"}), 64)
        self.assertEqual(self.flags({"cpu_no_unimplemented": "false", "fpu_no_unimplemented": "true"}), 32)
        self.assertEqual(self.flags({"cpu_no_unimplemented": True, "fpu_no_unimplemented": False}), 64)
        self.assertEqual(self.flags({"cpu_no_unimplemented": "yes", "fpu_no_unimplemented": "0"}), 64)

    def test_jit_fixup_declares_both_compatibility_policies(self):
        for cache in ("8192", 8192, "0x2000"):
            options = {"cachesize": cache, "cpu_no_unimplemented": "true", "fpu_no_unimplemented": "true",
                       "compfpu": "false"}
            self.assertEqual(self.flags(options, {"flags": "unsafe,noscreen"}), 1 | 16 | 32 | 64)
        self.assertEqual(self.flags({"cachesize": "0", "cpu_no_unimplemented": "true",
                                     "fpu_no_unimplemented": "true"}), 0)

    def test_late_config_overrides_and_network_flags_are_preserved(self):
        strict = {"cpu_no_unimplemented": "true", "fpu_no_unimplemented": "true"}
        self.assertEqual(self.flags(strict, {"flags": "net,noscreen"},
                                    extra_options={"fpu_no_unimplemented": "false"}), 8 | 16 | 64)
        self.assertEqual(self.flags({}, {"flags": "unsafe,intcompat,fpucompat"},
                                    extra_options=strict), 1)
        self.assertEqual(self.flags(strict, extra_options={"cachesize": "8192", "a2065": "slirp"}), 8 | 96)

    def test_invalid_options_cannot_silently_select_compatibility(self):
        for options in ({"cpu_no_unimplemented": "typo"}, {"fpu_no_unimplemented": "typo"},
                        {"cachesize": "typo"}):
            with self.subTest(options=options), self.assertRaises(runner.RunnerError):
                self.flags(options)

    def test_symbolic_flags_serialize_at_the_existing_payload_offset(self):
        self.assertEqual(runner.FLAG_BITS, mkadf.FLAG_BITS)
        for value in ("intcompat", "fpucompat", "unsafe+intcompat+fpucompat+intcompat", "0x60"):
            p = mkadf.default_params()
            mkadf.apply_param(p, "flags=" + value)
            blob = mkadf.pack_params(p)
            self.assertEqual(len(blob), 64)
            self.assertEqual(struct.unpack_from(">I", blob, 16)[0], p["flags"])
            self.assertEqual(mkadf.unpack_params(blob), p)
        self.assertEqual(mkadf.FLAG_BITS["intcompat"], 32)
        self.assertEqual(mkadf.FLAG_BITS["fpucompat"], 64)

    def test_runtime_declares_matching_policy_and_defaults_to_hardware_strict(self):
        header = (ROOT / "disk/rt/rt.h").read_text()
        for name, value in (("INTCOMPAT", 32), ("FPUCOMPAT", 64)):
            self.assertRegex(header, rf"(?m)^#define AMT_F_{name}\s+{value}\b")
        runtime = (ROOT / "disk/rt/main.c").read_text()
        defaults = runtime.split("static void load_params(void)", 1)[1].split("static void emit_hello", 1)[0]
        self.assertIn("amt_par.flags = 0;", defaults)
        hello = runtime.split('json_obj("policy");', 1)[1].split("json_obj_end();", 1)[0]
        for field, macro in (("integer", "INTCOMPAT"), ("fpu", "FPUCOMPAT")):
            self.assertRegex(hello, rf'json_str\("{field}",\s*\(amt_par.flags & AMT_F_{macro}\)'
                             r'\s*\?\s*"compat"\s*:\s*"strict"\);')
        self.assertIn("intcompat=32", cli.PARAM_HELP)
        self.assertIn("fpucompat=64", cli.PARAM_HELP)


if __name__ == "__main__":
    unittest.main()
