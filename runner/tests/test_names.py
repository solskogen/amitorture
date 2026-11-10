"""Every name a runner module uses is defined: a NameError on a path the
other tests do not reach (it happened: _launch_once used extra_options)."""
import builtins
import glob
import os
import symtable
import unittest

PKG = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "amitorture")


def undefined(path):
    with open(path, encoding="utf-8") as fh:
        top = symtable.symtable(fh.read(), path, "exec")
    defined = {s.get_name() for s in top.get_symbols() if s.is_assigned() or s.is_imported()}
    defined |= {"__file__", "__name__"}
    out = []

    def walk(t):
        for s in t.get_symbols():
            n = s.get_name()
            if s.is_referenced() and (s.is_global() or t.get_type() == "module") \
                    and n not in defined and not hasattr(builtins, n):
                out.append(f"{t.get_name()}: {n}")
        for c in t.get_children():
            walk(c)
    walk(top)
    return out


class Names(unittest.TestCase):
    def test_no_undefined_names(self):
        for path in sorted(glob.glob(os.path.join(PKG, "*.py"))):
            self.assertEqual(undefined(path), [], os.path.basename(path))


if __name__ == "__main__":
    unittest.main()
