#!/bin/sh
# Regenerate every generated table and fail if it differs from the
# checked-in copy (same idea as Amiberry's gencpu output: never let the
# generator and its output drift apart).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/work/tmp/check-generated.$$"
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
python3 "$ROOT/tools/gen_cpu_basic.py" --asm "$T/cpu_basic_tab.s" --doc "$T/cpu_basic.md" >/dev/null
python3 "$ROOT/tools/gen_fpu.py" -o "$T/fpu_gen.s" >/dev/null
python3 "$ROOT/tools/gen_blitter.py" --out "$T/blitter_tab.c" >/dev/null
python3 "$ROOT/tools/gencode.py" --asm "$T/dispatch_tab.s" --doc "$T/dispatch.md" >/dev/null
rc=0
for pair in "cpu_basic_tab.s:disk/modules/cpu_basic_tab.s" "fpu_gen.s:disk/modules/fpu_gen.s" \
            "blitter_tab.c:disk/modules/blitter_tab.c" "cpu_basic.md:docs/cases/cpu_basic.md" \
            "dispatch_tab.s:disk/modules/dispatch_tab.s" "dispatch.md:docs/cases/dispatch.md"; do
    new=${pair%%:*}; old=${pair#*:}
    if cmp -s "$T/$new" "$ROOT/$old"; then echo "ok    $old"; else echo "DRIFT $old (regenerate it)"; rc=1; fi
done
exit $rc
