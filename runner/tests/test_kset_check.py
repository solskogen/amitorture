"""tools/kset_check.py: the opcode screen for bench kernel sets."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import kset_check  # noqa: E402

GOOD_020 = """\
#NO_APP
	.text
_f:
	muls.l #1664525,d2
	divsl.l d5,d1:d0
	divul.l #65521,d0:d3
	divs.l d1,d7
	bfextu (a0){#3:#5},d0
	extb.l d1
	move.l (a0,d0.l*4),d1
	rts
"""

GOOD_FPU = """\
_g:
	fmove.s 4(sp),fp0
	fmul.x fp1,fp0
	fsub.x fp2,fp0
	fdiv.x fp3,fp0
	fsqrt.x fp0
	fcmp.s #0x3f800000,fp0
	fjngt .L2
	fmovem #28,-(sp)
.L2:	rts
"""


class KsetCheck(unittest.TestCase):
    def bad(self, text, kset):
        return kset_check.problems(text, kset)

    def test_clean_code_passes(self):
        self.assertEqual(self.bad(GOOD_020, "k020"), [])
        self.assertEqual(self.bad(GOOD_FPU, "k020f"), [])
        self.assertEqual(self.bad("_h:\n\tmove.l d0,d1\n\tmulu.w d2,d3\n\tjra _h\n", "k000"), [])

    def test_k000_rejects_68020_code(self):
        for line in GOOD_020.splitlines()[3:10]:
            with self.subTest(line=line):
                self.assertTrue(self.bad("_f:\n" + line + "\n", "k000"))

    def test_060_unimplemented_integer(self):
        for line in ("\tmulu.l d0,d1:d2", "\tmuls.l (a0),d3:d4", "\tdivu.l d0,d1:d2",
                     "\tdivs.l #3,d1:d2", "\tmovep.l d0,0(a0)", "\tcas2.l d0:d1,d2:d3,(a0):(a1)",
                     "\tchk2.l (a0),d0", "\tcmp2.w (a0),d1"):
            with self.subTest(line=line):
                self.assertTrue(self.bad(line + "\n", "k020"))
                self.assertTrue(self.bad(line + "\n", "k020f"))

    def test_fpu_only_in_k020f(self):
        self.assertTrue(self.bad(GOOD_FPU, "k020"))
        self.assertTrue(self.bad(GOOD_FPU, "k020s"))
        self.assertTrue(self.bad(GOOD_FPU, "k000"))

    def test_fpu_emulated_instructions(self):
        for line in ("\tfsin.x fp0", "\tfintrz.x fp0,fp1", "\tfetox.x fp1", "\tfmovecr #0,fp0",
                     "\tfseq d0", "\tfdbne d0,.L1", "\tftrapeq", "\tfmove.p fp0,(a0){#3}",
                     "\tfscale.l d0,fp0", "\tfrem.x fp1,fp0"):
            with self.subTest(line=line):
                self.assertTrue(self.bad(line + "\n", "k020f"))

    def test_main_reports_and_fails(self):
        tmp = ROOT / "work" / "tmp"
        tmp.mkdir(parents=True, exist_ok=True)
        f = tmp / "kset_check_bad.s"
        f.write_text("_f:\n\tmovep.w d0,2(a0)\n")
        try:
            import contextlib
            import io
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(kset_check.main(["--set", "k020", str(f)]), 1)
            self.assertIn("movep", err.getvalue())
            f.write_text("_f:\n\tmove.l d0,d1\n")
            self.assertEqual(kset_check.main(["--set", "k020", str(f)]), 0)
        finally:
            f.unlink()


if __name__ == "__main__":
    unittest.main()
