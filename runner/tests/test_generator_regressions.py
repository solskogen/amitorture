"""Model and mutation checks for the audited FSGLMUL, stack-pop and B-modulo cases."""
import copy
import importlib.util
from pathlib import Path
import random
import re
import unittest
from unittest import mock
import zlib


ROOT = Path(__file__).resolve().parents[2]


def load_generator(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SingleMultiply(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.f = load_generator("gen_fpu")

    def test_truncates_both_inputs_before_rounding_in_every_mode_and_precision(self):
        f = self.f
        tail = f.P2(-24)
        for mode in f.MODES:
            for prec in f.PRECS:
                for src, dst, expected in ((1 + tail, 1, 1), (1, 1 + tail, 1),
                                           (1 + tail, 1 + tail, 1),
                                           (-(1 + tail), 1, -1), (1, -(1 + tail), -1)):
                    with self.subTest(mode=mode, prec=prec, src=src, dst=dst):
                        result = f.op_sgl(f.fin(src), f.fin(dst), mode, prec, False)
                        self.assertEqual(f.x_enc(result.v), f.x_enc(f.fin(expected)))
                        self.assertEqual(result.exc, 0)
                        self.assertEqual(result.fpsr_dc, 0)
                        self.assertEqual(result.res_dc, (0, 0, 0))

    def test_preserves_extended_exponents_and_rounds_the_product(self):
        f = self.f
        for exponent in (-300, 200):
            result = f.op_sgl(f.fin((1 + f.P2(-24)) * f.P2(exponent)),
                              f.fin(1), f.RP, f.PD, False)
            self.assertEqual(f.x_enc(result.v), f.x_enc(f.fin(f.P2(exponent))))
            self.assertEqual(result.exc, 0)
        a = f.fin(1 + f.P2(-23))
        for mode in f.MODES:
            result = f.op_sgl(a, a, mode, f.PX, False)
            expected = 1 + f.P2(-22) + (f.P2(-23) if mode == f.RP else 0)
            self.assertEqual(f.x_enc(result.v), f.x_enc(f.fin(expected)))
            self.assertEqual(result.exc, f.INEX2)

    def test_old_full_mantissa_multiply_is_distinguishable(self):
        f = self.f
        src, dst = f.fin(1 + f.P2(-24)), f.fin(1)
        old = f.rnd(0, f.Exact(src.m * dst.m), f.RP, f.PS)
        fixed = f.op_sgl(src, dst, f.RP, f.PX, False)
        self.assertNotEqual(f.x_enc(old.v), f.x_enc(fixed.v))
        self.assertNotEqual(old.exc, fixed.exc)

    def test_generated_vectors_keep_strict_masks_and_all_applicable_fpus(self):
        f = self.f
        vectors = dict(f.GROUPS)["arith_fsglmul"]
        added = [v for v in vectors if "both input mantissas truncated" in v["why"]]
        self.assertEqual(len(added), 48)
        for v in added:
            self.assertEqual(v["mask"], f.MALL)
            self.assertEqual(v["tol"], 0)
            self.assertEqual(v["rmask"], (0xffffffff,) * 3)
            self.assertEqual(v["fpsr_mask"], 0xffffffff)
            expected = f.fin(-1 if v["src"][0] >> 31 else 1)
            self.assertEqual(v["res"], f.x_enc(expected))
            self.assertEqual(v["fpsr"], f.CC_N if expected.s else 0)

    def test_division_and_special_value_rules_are_unchanged(self):
        f = self.f
        for mode in f.MODES:
            result = f.op_sgl(f.fin(3), f.fin(1), mode, f.PD, True)
            expected = f.rnd(0, f.Exact(f.Fr(1, 3)), mode, f.PS)
            self.assertEqual(f.x_enc(result.v), f.x_enc(expected.v))
            self.assertEqual(result.exc, expected.exc)
        for src, dst in ((f.ZERO, f.fin(-1)), (f.INF, f.ZERO),
                         (f.SNANV, f.fin(1)), (f.NZERO, f.NINF)):
            result = f.op_sgl(src, dst, f.RP, f.PS, False)
            expected = f.op_mul(src, dst, f.RP, f.PX)
            self.assertEqual(f.x_enc(result.v), f.x_enc(expected.v))
            self.assertEqual(result.exc, expected.exc)
            self.assertEqual(result.fpsr_dc, expected.fpsr_dc)
            self.assertEqual(result.res_dc, expected.res_dc)


class DispatchStack(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.g = load_generator("gencode")

    def test_move_push_pop_reads_before_releasing_stack_bytes(self):
        g = self.g
        for cpu in range(len(g.CLASSES)):
            for size in (1, 2, 4):
                with self.subTest(cpu=cpu, size=size):
                    st = g.init_state(0x12345679, cpu)
                    initial = st.r[15]
                    step = 2 if size == 1 else size
                    value = 0x89abcdef & g.msk(size)
                    g.lwr(st, g.loc(st, g.EA("pd", 15), size), size, value)
                    self.assertEqual(st.r[15], initial - step)
                    location = g.loc(st, g.EA("pi", 15), size)
                    self.assertEqual(st.r[15], initial - step)
                    self.assertEqual(g.lrd(st, location, size), value)
                    self.assertEqual(st.r[15], initial)
                    self.assertFalse(any(a < initial for a in st.stk))
                    self.assertEqual(st.stk[initial], ("H", None, 0))

    def test_old_eager_postincrement_rejects_all_move_pop_sizes(self):
        g = self.g
        for size in (1, 2, 4):
            st = g.init_state(1, 0)
            g.lwr(st, g.loc(st, g.EA("pd", 15), size), size, 0x12345678)
            address = st.r[15]
            g.set_ptr(st, 15, address + (2 if size == 1 else size))
            with self.assertRaises(g.Bad):
                g.mrd(st, address, size)

    def test_postincrement_write_and_read_modify_write_commit_after_access(self):
        g = self.g
        for size in (1, 2, 4):
            for reg in (8, 15):
                with self.subTest(size=size, reg=reg):
                    st = g.init_state(1, 0)
                    if reg == 15:
                        g.set_ptr(st, reg, st.r[reg] - 8)
                    else:
                        st.r[reg] = 32
                    base = st.r[reg]
                    step = 2 if reg == 15 and size == 1 else size
                    g.mwr(st, base, size, 3)
                    location = g.loc(st, g.EA("pi", reg), size)
                    self.assertEqual(g.lrd(st, location, size, rmw=True), 3)
                    self.assertEqual(st.r[reg], base)
                    g.lwr(st, location, size, 4)
                    self.assertEqual(st.r[reg], base + step)
                    if reg == 8:
                        self.assertEqual(g.mrd(st, base, size), 4)
                    base = st.r[reg]
                    g.lwr(st, g.loc(st, g.EA("pi", reg), size), size, 5)
                    self.assertEqual(st.r[reg], base + step)
                    if reg == 8:
                        self.assertEqual(g.mrd(st, base, size), 5)

    def test_uninitialised_and_harness_stack_reads_still_fail(self):
        g = self.g
        st = g.init_state(1, 0)
        with self.assertRaises(g.Bad):
            g.lrd(st, g.loc(st, g.EA("pi", 15), 4), 4)
        g.set_ptr(st, 15, st.r[15] - 4)
        with self.assertRaises(g.Bad):
            g.lrd(st, g.loc(st, g.EA("pi", 15), 4), 4)

    def test_movem_and_rts_keep_their_separate_stack_paths(self):
        g = self.g
        for size in (2, 4):
            st = g.init_state(0x12345679, 0)
            initial = st.r[:]
            gen = g.Gen(random.Random(1), 0)
            with mock.patch.object(gen, "p", return_value=False), \
                    mock.patch.object(gen, "ch", return_value=size), \
                    mock.patch.object(gen.rng, "sample", return_value=[0, 1, 12]), \
                    mock.patch.object(g, "body", return_value=[]):
                instructions, _ = g.item_stack(gen, st)
            for instruction in instructions:
                instruction.ex(st)
            for reg in (0, 1, 12):
                self.assertEqual(st.r[reg], g.u32(g.sx(initial[reg], size)))
            self.assertEqual(st.r[15], initial[15])
            self.assertFalse(any(a < st.r[15] for a in st.stk))
            self.assertIs(g.ex_rts(st), g.END)
            self.assertEqual(st.r[15], g.STK_TOP)

    def test_all_generated_classes_include_each_ordinary_move_pop_size(self):
        g = self.g
        assembled = (ROOT / "disk/modules/dispatch_tab.s").read_text()
        for cpu in range(len(g.CLASSES)):
            sizes = set()
            for group in range(g.GROUPS):
                for block in range(g.NBLK):
                    _, prog, _, _, _ = g.make_block(cpu, group, block)
                    for ins in prog:
                        match = re.fullmatch(r"\tmove\.([bwl])\t\(sp\)\+,d[0-7]", ins.text)
                        if match:
                            sizes.add(match[1])
                            self.assertIn(ins.text, assembled)
            self.assertEqual(sizes, {"b", "w", "l"}, g.CLASSES[cpu][0])


class NegativeBModulo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.b = load_generator("gen_blitter")
        cls.case = next(c for c in cls.b.build_cases() if c.id == "mod_negative_b")

    def observe(self, case):
        b = self.b
        mem = b.Mem()
        # Give wrong pointer walks deterministic backing too. Reapply the real
        # fills last, so the correct vector is byte-identical to the guest table.
        mem.fill(b.B_BASE, 128, 0x87654321)
        for off, words, seed in case.fills:
            mem.fill(off, words, seed)
        zero = case.run(mem)
        off, size = case.window
        output = bytes(mem.b[off:off + size])
        return output, zlib.crc32(output) & 0xffffffff, int(zero)

    def test_negative_modulo_differs_from_zero_positive_and_disabled_b_dma(self):
        b = self.b
        original = self.case.evaluate()
        expected = self.observe(original)
        self.assertEqual(expected, (original.expect, original.crc, original.zero))
        for modulo in (0, 12, -8):
            mutated = copy.copy(original)
            mutated.bmod = modulo
            with self.subTest(modulo=modulo):
                observed = self.observe(mutated)
                self.assertNotEqual(observed[0], expected[0])
                self.assertNotEqual(observed[1], expected[1])
        mutated = copy.copy(original)
        mutated.con0 &= ~b.USEB
        self.assertNotEqual(self.observe(mutated)[1], expected[1])

    def test_original_d_equals_a_minterm_is_insensitive_to_b(self):
        b = self.b
        old = copy.copy(self.case)
        old.con0 = (old.con0 & ~0xff) | 0xf0
        expected = self.observe(old)
        self.assertEqual(expected[1], 0x6b0347d1)
        for modulo in (0, 12, -8):
            mutated = copy.copy(old)
            mutated.bmod = modulo
            self.assertEqual(self.observe(mutated), expected)
        mutated = copy.copy(old)
        mutated.con0 &= ~b.USEB
        self.assertEqual(self.observe(mutated), expected)
