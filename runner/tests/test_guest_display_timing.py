"""Native checks of the guest's sampling verdict and display scan helpers."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^static [^\n]+\b" + name + r"\([^;]*?\)\s*\{",
                      source, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing guest helper {name}")
    start = match.start()
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class GuestDisplayTiming(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler unavailable")
        cls.build = ROOT / "work" / f"guest-display-timing-{os.getpid()}"
        cls.build.mkdir(parents=True, exist_ok=True)
        cls.addClassCleanup(shutil.rmtree, cls.build)
        cls.binary = cls.build / "oracles"
        display = (ROOT / "disk/modules/display.c").read_text()
        timing = (ROOT / "disk/modules/timing.c").read_text()
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
enum { AMT_ST_PASS, AMT_ST_FAIL, AMT_ST_UNTESTABLE };
#define NPL 48
#define PY(i) (0x48 + pstep * (i))
#define SF_CUSTSPR 0x20
struct probe { UWORD h; UBYTE fine, dy; UWORD data; };
static struct probe pr[NPL][4];
static struct { UWORD sprctl_or, flags, con0; } sc;
static int aga;
static UWORD storage[8][512], *spr_list[8];
static UWORD pixels[8][80], *plane[8], pr_clx[NPL][8];
static UBYTE pr_col[NPL][4];
static int pstep = 3, pr_n = 5, selecting, group;
static const UBYTE cells[20] = {
    0,1,2,4,8,16,32,64,96,42,13,94,10,83,59,65,2,41,19,127
};
static UWORD clean_clx(UWORD clx, UWORD clx2, int flags);
"""
        source += "\n".join(function(display, name) for name in
                            ("build_group", "clx_plane", "scan_mask",
                             "fill_plane_words", "fill_cells"))
        source += function(timing, "hscan_status")
        source += r"""
static UWORD clean_clx(UWORD clx, UWORD clx2, int flags)
{
    int p, s, first = 0, slots = 4;
    UWORD result = 0;
    if (sc.con0 & 0x40) {
        p = selecting % 2;
        group = selecting / 4;
        first = ((selecting / 2) % 2) * 2;
        slots = 2;
        selecting++;
        assert(storage[4][0] == 0 && storage[6][0] == 0);
        for (s = 0; s < slots; s++)
            assert((storage[2*s][0] & 255) == ((pr[group][first+s].h >> 1) & 255));
    } else {
        p = selecting++ % 7;
        if (!p) group++;
    }
    assert(!flags);
    assert(clx == (p < 6 ? ((0x40 << p) | (1 << p)) : 0));
    assert(clx2 == (p == 6 ? 0x41 : 0));
    for (s = 0; s < slots; s++)
        if (cells[4 * group + first + s] & (1 << p))
            result |= ((p & 1) ? 0x20 : 2) << s;
    return result;
}
int main(int argc, char **argv)
{
    int p, c, slot;
    assert(argc == 2);
    switch (argv[1][0]) {
    case 'h':
        for (int pal = 0; pal < 2; pal++) {
            ULONG lines = 5000, exact = pal ? 227000 : 227500;
            UWORD mx = pal ? 0xe2 : 0xe3;
            assert(hscan_status(0xdf, mx, lines, exact, exact, pal) == AMT_ST_PASS);
            assert(hscan_status(mx, mx, lines, exact - 20, exact + 20, pal) == AMT_ST_PASS);
            assert(hscan_status(mx + 1, mx, lines, exact, exact, pal) == AMT_ST_FAIL);
            for (int delta = -2000; delta <= 2000; delta += 500)
                if (delta)
                    assert(hscan_status(mx, mx, lines, exact + delta,
                                        exact + delta, pal) == AMT_ST_FAIL);
            assert(hscan_status(mx, mx, lines, exact - 300, exact + 300, pal) == AMT_ST_FAIL);
            for (int sign = -1; sign <= 1; sign += 2) {
                ULONG inside = exact + sign * 203, outside = exact + sign * 204;
                assert(hscan_status(mx, mx, lines, inside, inside, pal) == AMT_ST_PASS);
                assert(hscan_status(mx, mx, lines, outside, outside, pal) == AMT_ST_FAIL);
            }
            assert(hscan_status(mx, mx, 0, exact, exact, pal) == AMT_ST_FAIL);
            assert(hscan_status(mx, mx, lines, exact + 1, exact, pal) == AMT_ST_FAIL);
        }
        break;
    case 'f':
        for (p = 0; p < 8; p++) plane[p] = pixels[p];
        for (slot = 1; slot <= 4; slot *= 2) {
            memset(pixels, 0xa5, sizeof(pixels));
            assert(fill_cells(7, slot, 20, cells) == 40 * slot);
            for (p = 0; p < 8; p++)
                for (c = 0; c < 20 * slot; c++)
                    assert(pixels[p][c] ==
                           ((p < 7 && (cells[c / slot] & (1 << p))) ? 0xffff : 0));
        }
        break;
    case 'e':
        sc.con0 = 0x2240;
        /* fall through */
    case 's':
        memset(storage, 0xa5, sizeof(storage));
        for (p = 0; p < 8; p++) spr_list[p] = storage[p];
        for (c = 0; c < 20; c++) {
            pr[c / 4][c % 4].h = 0x80 + 16 * c;
            pr[c / 4][c % 4].data = 0x180;
        }
        group = -1;
        scan_mask(sc.con0 ? 3 : 0x7f);
        assert(selecting == (sc.con0 ? 20 : 35));
        for (c = 0; c < 20; c++)
            assert(pr_col[c / 4][c % 4] == (cells[c] & (sc.con0 ? 3 : 0x7f)));
        if (!sc.con0) {
            assert(pr_clx[1][5] == 0x80); /* only cell 6, not cells 4,5,7 */
            assert(pr_clx[1][6] == 0x10); /* only cell 7 */
        }
        for (p = 0; p < (sc.con0 ? 4 : 8); p += 2) {
            assert(storage[p][2] == 0x180 && storage[p][3] == 0);
            assert(storage[p][4] == 0 && storage[p][5] == 0);
        }
        break;
    default: assert(0);
    }
    return 0;
}
"""
        result = subprocess.run(
            [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-x", "c",
             "-", "-o", str(cls.binary)], input=source, text=True,
            capture_output=True, env={**os.environ, "TMPDIR": str(cls.build)})
        if result.returncode:
            raise AssertionError(result.stderr)

    def test_horizontal_sampling_verdict(self):
        subprocess.run([self.binary, "h"], check=True)

    def test_seven_plane_row_overwrites_poison(self):
        subprocess.run([self.binary, "f"], check=True)

    def test_seven_plane_collision_selectors_and_decode(self):
        subprocess.run([self.binary, "s"], check=True)

    def test_ecs_shres_preserves_all_spatial_samples_on_available_sprites(self):
        subprocess.run([self.binary, "e"], check=True)
