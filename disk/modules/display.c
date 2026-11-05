/* display: Denise/Lisa playfields, sprites, collisions and AGA extensions (CONTRACT.md section 10, module id 12, disk "chip").
 *
 * Expectations: Amiga Hardware Reference Manual (HRM) chapters 3
 * (playfields), 4 (sprites), 7 (collisions) and appendix C
 * (ECS), and the AGA register documentation (FMODE, BPLCON3/4, CLXCON2,
 * LISAID, RDRAM). See docs/cases/display.md.
 *
 * The oracle is the collision hardware. A "probe" is a one-line DMA sprite
 * (sprites 0, 2, 4, 6; the odd ones point to an empty list) placed over the
 * playfield. With CLXCON enabling one bitplane with match value 1, CLXDAT
 * bit 1+n (odd plane) / 5+n (even plane) tells whether any playfield pixel
 * under sprite 2n has that plane bit set. Reads are timing-independent:
 * the scene is static from frame to frame, the CPU changes CLXCON/sprite
 * lists at a vertical blank, throws away CLXDAT at the next one and reads
 * it two frames later (clean_clx): the OR of two complete frames. One probe
 * group (4 sprites) and one plane per measurement; the colour index under
 * each probe is assembled from one measurement per plane. Fetch widths,
 * modulos, scrolling, window edges and sprite widths are all turned into
 * "which colour is under this probe". Frame length, LOF and the H counter
 * range are timing's lines_per_frame_* and cck_per_line.
 *
 * Screenshots (CONTRACT section 13) are requested for each display mode
 * pattern; the result is informational, the case status comes only from
 * what the CPU can observe. */
#include "mod.h"
#include "display_hw.h"

void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);

AMT_MODULE(display, 12, AMT_PH_BARE, AMT_MF_LONG | AMT_MF_OWNS_DISPLAY, 98304, 0);

/* ------------------------------------------------------------ layout */
#define PL_BYTES   0x2000            /* per bitplane buffer (8 of them) */
#define OFF_COP    0x10000           /* 2 copper lists */
#define COP_BYTES  0x1000
#define OFF_SPR    0x12000           /* 8 sprite lists */
#define SPR_BYTES  0x400
#define OFF_NULL   0x14000           /* empty sprite list */
#define OFF_WORK   0x14100

#define PY0     0x48                 /* first probe line */
#define NPL     48                   /* probe lines per frame (to $D5) */
#define PY(i)   (PY0 + pstep * (i))
#define DV0     0x40                 /* default DIW vertical start */
#define DV1     0xf0                 /* default DIW vertical stop */

static UBYTE *pool;
static int pstep = 3;                /* probe line spacing (rows in AE patterns) */
static int aga, ecsd, ecsa, pal;
static UWORD *plane[8];
static UWORD *spr_list[8];
static UWORD *spr_null;
static int copi;
static int host_ok = 1;              /* screenshots: cleared at the first timeout */
static ULONG shot_n;

/* ------------------------------------------------------------ scene */
#define SF_DIWHIGH 0x01
#define SF_PAL32   0x02
#define SF_PAL256  0x04
#define SF_NOSPR   0x08              /* sprite DMA off (manual mode tests) */
#define SF_CUSTSPR 0x20              /* sprite lists built by the test */
#define SF_ODDSPR  0x40              /* odd sprites use their own lists too */

struct scene {
    UWORD con0, con1, con2, con3, con4, fmode;
    UWORD diwstrt, diwstop, ddfstrt, ddfstop, diwhigh;
    WORD mod1, mod2;
    UWORD flags;
    UWORD sprctl_or;                 /* ORed into probe SPRxCTL (AGA SH bits) */
    const UWORD *extra;              /* extra raw copper words after setup */
    int nextra;
};

static struct scene sc;
static UWORD pal32[32];
static ULONG pal256[256];
static UWORD ptr_off;                /* plane pointers = plane[p] + ptr_off words */

/* probes: per probe line, 4 slots (sprites 0, 2, 4, 6) */
struct probe {
    UWORD h;          /* lores H position (SPRxPOS/CTL H8-H0) */
    UBYTE fine;       /* AGA/ECS SH bits (CTL bits 4,3), shres units 0..3 */
    UBYTE dy;         /* line offset 0..1 inside the probe window */
    UWORD data;       /* SPRxDATA (0 = no probe); DATB = 0 */
};
static struct probe pr[NPL][4];
static int pr_n;
static UBYTE pr_col[NPL][4];
static UWORD pr_clx[NPL][8];
static UWORD scan_tries;

static void reset_scene(void)
{
    memset(&sc, 0, sizeof(sc));
    ptr_off = 0;
    pstep = 3;
    sc.con0 = 0x0200;
    sc.con3 = 0x0c00;
    sc.con4 = 0x0011;
    sc.diwstrt = (DV0 << 8) | 0x81;
    sc.diwstop = ((DV1 & 0xff) << 8) | 0xc1;
    sc.ddfstrt = 0x38;
    sc.ddfstop = 0xd0;
}

static void reset_probes(void)
{
    memset(pr, 0, sizeof(pr));
    pr_n = 0;
}

static UWORD *cw;
static void cm(UWORD reg, UWORD val) { *cw++ = reg; *cw++ = val; }
static void __attribute__((unused)) cwait(UWORD v, UWORD h) { *cw++ = (UWORD)(((v & 0xff) << 8) | (h & 0xfe) | 1); *cw++ = 0xfffe; }
static void cptr(UWORD reg, const void *p) { cm(reg, (UWORD)((ULONG)p >> 16)); cm(reg + 2, (UWORD)(ULONG)p); }

/* sprite lists for probe group i only: sprite 2s shows slot s on line
 * PY(i)+dy; empty lists for slots without a probe */
static void build_group(int i, int first, int count)
{
    int s;
    for (s = 0; s < 4; s++) {
        UWORD *sl = spr_list[s * 2];
        struct probe *p = &pr[i][first + (s % count)];
        if (s < count && p->data) {
            UWORD y = (UWORD)(PY(i) + p->dy);
            *sl++ = (UWORD)((y << 8) | ((p->h >> 1) & 0xff));
            *sl++ = (UWORD)(((y + 1) << 8) | (p->h & 1) | ((p->fine & 2) << 3) |
                            ((p->fine & 1) << 3) | sc.sprctl_or);
            *sl++ = p->data;
            *sl++ = 0;
        }
        *sl++ = 0;
        *sl++ = 0;
    }
}

static void cop_setup_regs(void)
{
    int i, npt = aga ? 8 : 6;
    cm(DIWSTRT, sc.diwstrt);
    cm(DIWSTOP, sc.diwstop);
    if ((sc.flags & SF_DIWHIGH) && ecsa)
        cm(DIWHIGH, sc.diwhigh);
    cm(DDFSTRT, sc.ddfstrt);
    cm(DDFSTOP, sc.ddfstop);
    if (aga)
        cm(FMODE, sc.fmode);
    cm(BPLCON0, sc.con0);
    cm(BPLCON1, sc.con1);
    cm(BPLCON2, sc.con2);
    if (ecsd)
        cm(BPLCON3, sc.con3);
    if (aga)
        cm(BPLCON4, sc.con4);
    cm(BPL1MOD, (UWORD)sc.mod1);
    cm(BPL2MOD, (UWORD)sc.mod2);
    for (i = 0; i < npt; i++)
        cptr((UWORD)(BPL1PTH + 4 * i), plane[i] + ptr_off);
    for (i = 0; i < 8; i++)
        cptr((UWORD)(SPR0PTH + 4 * i),
             ((i & 1) && !(sc.flags & SF_ODDSPR)) ? spr_null : spr_list[i]);
}

static void cop_palette(void)
{
    int i, b;
    if (sc.flags & SF_PAL32)
        for (i = 0; i < 32; i++)
            cm((UWORD)(COLOR00 + 2 * i), pal32[i]);
    if ((sc.flags & SF_PAL256) && aga) {
        for (b = 0; b < 8; b++) {
            cm(BPLCON3, (UWORD)((sc.con3 & 0x1dff) | (b << 13)));
            for (i = 0; i < 32; i++) {
                ULONG c = pal256[b * 32 + i];
                cm((UWORD)(COLOR00 + 2 * i), (UWORD)(((c >> 12) & 0xf00) | ((c >> 8) & 0xf0) | ((c >> 4) & 0xf)));
            }
            cm(BPLCON3, (UWORD)((sc.con3 & 0x1dff) | (b << 13) | 0x0200));
            for (i = 0; i < 32; i++) {
                ULONG c = pal256[b * 32 + i];
                cm((UWORD)(COLOR00 + 2 * i), (UWORD)(((c >> 8) & 0xf00) | ((c >> 4) & 0xf0) | (c & 0xf)));
            }
        }
        cm(BPLCON3, sc.con3);
    }
}

static int wait_vbl(void)
{
    ULONG f = amt_frames_now();
    ULONG budget = 1000000, t0 = amt_ticks();
    UWORD last = hw_vline(), v;
    int wraps = 0;
    while (amt_frames_now() == f) {
        if (!--budget || amt_ticks() - t0 > 4 * 71051UL) {
            amt_set_sr(0x2000);
            amt_abort(AMT_ABORT_WATCHDOG);
        }
        v = hw_vline();
        if (v < last && ++wraps > 3)
            return 0;
        last = v;
    }
    return 1;
}

static void wait_frames(int n)
{
    while (n-- > 0)
        wait_vbl();
}

/* build the copper list for the current scene + probes and start it */
static void apply(void)
{
    UWORD *base;
    int i;
    copi ^= 1;
    base = (UWORD *)(pool + OFF_COP + copi * COP_BYTES);
    cw = base;
    if (!(sc.flags & SF_CUSTSPR)) {
        int k;
        for (k = 0; k < 8; k++)
            spr_list[k][0] = spr_list[k][1] = 0;
    }
    cop_setup_regs();
    cop_palette();
    for (i = 0; i < sc.nextra; i++)
        *cw++ = sc.extra[i];
    *cw++ = 0xffff;
    *cw++ = 0xfffe;
    CUL(COP1LCH) = (ULONG)base;
    CU(COPJMP1) = 0;
    CU(DMACON) = (sc.flags & SF_NOSPR) ? DMAF_SPREN : 0;
    CU(DMACON) = (UWORD)(DMAF_SET | DMAF_DMAEN | DMAF_BPLEN | DMAF_COPEN |
                         ((sc.flags & SF_NOSPR) ? 0 : DMAF_SPREN));
    wait_frames(2);
}

static void display_off(void)
{
    CU(DMACON) = DMAF_BPLEN | DMAF_SPREN | DMAF_COPEN;
    CU(CLXCON) = 0;
}

static void wait_line(UWORD y)
{
    ULONG budget = 1000000;
    while (hw_vline() < y)
        if (!--budget) {
            amt_set_sr(0x2000);
            amt_abort(AMT_ABORT_WATCHDOG);
        }
}

/* Timing-independent collision read. The scene is static from frame to
 * frame, so: set CLXCON (and whatever the caller changed) at a VBL; the
 * CLXDAT read at the next DISP_DISCARD VBL(s) throws away the partly-set-up
 * frame and anything a core still delivers late; the read two VBLs later returns the
 * OR of two complete, fully set up frames. A CPU that reads late only adds
 * collisions of identical frames. */
#define FC_REVERSE 1                  /* write CLXCON2 before CLXCON */
/* Frames thrown away after a change. 3 were used first, against cores that
 * deliver a frame's collisions late; an A/B on 4 machines (a1200, a4000
 * fast, AROS, a500 cycle-exact), 3 rounds each, gave the same results with
 * 1 (952 pass / 32 fail / 27 xfail both, the intermittents at the same
 * rate) and the module 30 % faster (193 s -> 135 s). */
#ifndef DISP_DISCARD
#define DISP_DISCARD 1
#endif
static UWORD clean_clx(UWORD clx, UWORD clx2, int flags)
{
    UWORD r;
    wait_vbl();
    if (flags & FC_REVERSE) {
        if (aga)
            CU(CLXCON2) = clx2;
        CU(CLXCON) = clx;
    } else {
        CU(CLXCON) = clx;
        if (aga)
            CU(CLXCON2) = clx2;
    }
    /* DISP_DISCARD frames thrown away (a core may draw, and so collide, a
     * frame well after the beam passed it), then two frames accumulated */
    for (r = 0; r < DISP_DISCARD; r++) {
        wait_vbl();
        (void)CU(CLXDAT);
    }
    wait_vbl();
    wait_vbl();
    r = CU(CLXDAT) & 0x7fff;
    scan_tries++;
    return r;
}

/* CLXCON for "plane p (0-based) bit = 1" */
static UWORD clx_plane(int p) { return (UWORD)((0x40 << p) | (1 << p)); }

/* Read the colour index under every probe, for the planes in `mask`:
 * pr_col. One probe group (4 sprites) at a time; one clean frame per plane,
 * with CLXCON enabling only that plane (match 1), so the other playfield
 * group has no enabled plane and always matches: the read-out does not
 * depend on how odd/even matches combine (see clx_spf_odd_independent). */
static void scan_mask(UBYTE mask)
{
    int p, i, s, first;
    int slots = !aga && (sc.con0 & 0x40) && !(sc.flags & SF_CUSTSPR) ? 2 : 4;
    memset(pr_col, 0, sizeof(pr_col));
    memset(pr_clx, 0, sizeof(pr_clx));
    for (i = 0; i < pr_n; i++) {
        if (!(pr[i][0].data | pr[i][1].data | pr[i][2].data | pr[i][3].data))
            continue;
        for (first = 0; first < 4; first += slots) {
            if (!(sc.flags & SF_CUSTSPR))
                build_group(i, first, slots);
            for (p = 0; p < 8; p++) {
                UWORD clx = 0, clx2 = 0, bit = (p & 1) ? 0x20 : 0x02, r;
                if (!((mask >> p) & 1))
                    continue;
                if (p < 6)
                    clx = clx_plane(p);
                else
                    clx2 = (p == 6) ? 0x41 : 0x82;
                r = clean_clx(clx, clx2, 0);
                pr_clx[i][p] |= r;
                for (s = 0; s < slots; s++)
                    if (pr[i][first + s].data && (r & (bit << s)))
                        pr_col[i][first + s] |= (UBYTE)(1 << p);
            }
        }
    }
}

static void scan_colors(int np) { scan_mask((UBYTE)((1 << np) - 1)); }

/* ------------------------------------------------------------ helpers */
static void fill_plane_words(int p, int start, int n, UWORD v)
{
    UWORD *q = plane[p] + start;
    while (n-- > 0)
        *q++ = v;
}

/* one row of ncell cells, wpc words each; cell c has colour col[c].
 * Returns the row length in bytes. */
static int fill_cells(int np, int wpc, int ncell, const UBYTE *col)
{
    int p, c;
    for (p = 0; p < 8; p++)
        for (c = 0; c < ncell; c++)
            fill_plane_words(p, c * wpc, wpc, (p < np && ((col[c] >> p) & 1)) ? 0xffff : 0);
    return ncell * wpc * 2;
}

/* a full standard lores row (20 cells of 16 px), cell c = col[c % n];
 * sets both modulos to repeat it on every line */
static void lores_row(int np, int n, const UBYTE *col)
{
    UBYTE c20[20];
    int c;
    for (c = 0; c < 20; c++)
        c20[c] = col[c % n];
    sc.mod1 = sc.mod2 = (WORD)-fill_cells(np, 1, 20, c20);
}

/* address-encoded planes: word w of plane p = bit p of w, for nwords */
static void fill_ae(int np, int nwords)
{
    int p, w;
    for (p = 0; p < 8; p++) {
        UWORD *q = plane[p];
        for (w = 0; w < nwords; w++)
            q[w] = (p < np && ((w >> p) & 1)) ? 0xffff : 0;
    }
}

static int shot(const char *tag)
{
    if (!host_ok)
        return -1;
    shot_n++;
    if (amt_request("screenshot", tag, "{\"frames\":2}", amt_secs(10)))
        return 1;
    host_ok = 0;
    return 0;
}

static void json_shot(int r)
{
    json_str("shot", r > 0 ? "acked" : r == 0 ? "not_acked" : "no_host");
}

static const char *const res_name[3] = { "lores", "hires", "shres" };

/* pf origin (sprite H of playfield x = 0) per resolution, lores units;
 * measured by calib_origin(), default $81 */
static UWORD origin[3] = { 0x81, 0x81, 0x81 };
static int origin_ok[3];

static void std_ddf(int res)
{
    if (res == 0) {
        sc.ddfstrt = 0x38;
        sc.ddfstop = 0xd0;
    } else {
        sc.ddfstrt = 0x3c;
        sc.ddfstop = 0xd4;
    }
}

static UWORD res_con0(int res, int np)
{
    UWORD c = (UWORD)(0x0200 | ((np & 7) << 12) | ((np & 8) ? 0x0010 : 0));
    if (res == 1)
        c |= 0x8000;
    else if (res == 2)
        c |= 0x0040;
    return c;
}

/* words per line per plane for the standard DDF: 20 lores, 40 hires, 80 shres */
static int std_words(int res) { return 20 << res; }

static void default_palette(void)
{
    int i;
    for (i = 0; i < 32; i++)
        pal32[i] = (UWORD)(((i * 7 + 3) & 0xf) << 8 | ((i * 5 + 9) & 0xf) << 4 | ((i * 11 + 1) & 0xf));
    pal32[0] = 0x0000;
    for (i = 0; i < 256; i++)
        pal256[i] = ((ULONG)((i * 37 + 5) & 0xff) << 16) | ((ULONG)((i * 101 + 77) & 0xff) << 8) |
                    (ULONG)((i * 13 + 200) & 0xff);
    pal256[0] = 0;
}

/* ------------------------------------------------------------ reporting */
static void json_bytes(const char *key, const UBYTE *b, int n)
{
    int i;
    json_arr(key);
    for (i = 0; i < n; i++)
        json_hexn(NULL, b[i], 2);
    json_arr_end();
}

static void json_scene(void)
{
    json_obj("scene");
    json_hexn("bplcon0", sc.con0, 4);
    json_hexn("bplcon1", sc.con1, 4);
    json_hexn("bplcon2", sc.con2, 4);
    if (ecsd)
        json_hexn("bplcon3", sc.con3, 4);
    if (aga) {
        json_hexn("bplcon4", sc.con4, 4);
        json_hexn("fmode", sc.fmode, 4);
    }
    json_hexn("diwstrt", sc.diwstrt, 4);
    json_hexn("diwstop", sc.diwstop, 4);
    if (sc.flags & SF_DIWHIGH)
        json_hexn("diwhigh", sc.diwhigh, 4);
    json_hexn("ddfstrt", sc.ddfstrt, 4);
    json_hexn("ddfstop", sc.ddfstop, 4);
    json_int("bpl1mod", sc.mod1);
    json_int("bpl2mod", sc.mod2);
    json_obj_end();
}

static void json_scan_ctx(void)
{
    json_obj("timing");
    json_str("unit", "clean collision frames measured so far in this module");
    json_int("scan_frames", scan_tries);
    json_obj_end();
}

/* ------------------------------------------------------------ calibration */
/* 1-pixel probes across a 0->1 edge at playfield x = 160 (lores units) */
static int edge_scan(int res, int np_plane, UWORD hfrom, int n, int *first)
{
    int j, f = -1, mono = 1;
    reset_probes();
    for (j = 0; j < n; j++) {
        struct probe *p = &pr[j / 4][j % 4];
        p->h = (UWORD)(hfrom + j);
        p->data = 0x8000;
    }
    pr_n = (n + 3) / 4;
    apply();
    scan_mask((UBYTE)(1 << np_plane));
    for (j = 0; j < n; j++) {
        int hit = (pr_col[j / 4][j % 4] >> np_plane) & 1;
        if (hit && f < 0)
            f = j;
        if (!hit && f >= 0)
            mono = 0;
    }
    *first = f;
    (void)res;
    return mono;
}

static void calib_origin(int res)
{
    static const char *const ids[3] = { "align_lores", "align_hires", "align_shres" };
    int w = std_words(res), first, mono, rb;
    UBYTE col[2] = { 0, 1 };
    reset_scene();
    sc.con0 = res_con0(res, 1);
    std_ddf(res);
    rb = fill_cells(1, w / 2, 2, col);
    sc.mod1 = sc.mod2 = (WORD)-rb;
    mono = edge_scan(res, 0, 0x81 + 160 - 16, 32, &first);
    display_off();
    if (first >= 0 && mono) {
        origin[res] = (UWORD)(0x81 + 160 - 16 + first - 160);
        origin_ok[res] = 1;
    }
    amt_case_open(ids[res], (first >= 0 && mono) ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!(first >= 0 && mono)) {
        json_obj("expect");
        json_str("edge", "one 0->1 step in 32 one-pixel probes around playfield x=160");
        json_obj_end();
        json_obj("got");
        json_int("first_hit", first);
        json_int("monotonic", mono);
        json_obj_end();
        json_str("why", "a playfield whose plane 1 is 0 for x<160 and 1 for x>=160 (standard DDF/DIW) "
                 "must collide with a one-pixel sprite from one H position on and never before "
                 "(HRM 7: collisions; 3: data fetch)");
    }
    json_obj("ctx");
    json_hexn("origin", origin[res], 3);
    json_str("note", "origin = sprite H whose first pixel sits on playfield x=0; HRM: DIW HSTART $81 "
             "matches DDFSTRT $38 (lores) / $3C (hires)");
    json_obj_end();
    json_scene();
    amt_case_close();
}

/* ------------------------------------------------------------ modes */
static UBYTE cellcol[20];

static void cell_probes(int res, int ncell)
{
    int c;
    reset_probes();
    for (c = 0; c < ncell; c++) {
        struct probe *p = &pr[c / 4][c % 4];
        p->h = (UWORD)(origin[res] + 16 * c);
        p->data = 0x0180;
    }
    pr_n = (ncell + 3) / 4;
}

static int check_cells(int ncell, UBYTE mask, int *badc)
{
    int c;
    for (c = 0; c < ncell; c++)
        if ((pr_col[c / 4][c % 4] & mask) != (cellcol[c] & mask)) {
            *badc = c;
            return 0;
        }
    return 1;
}

static void got_cells(int ncell)
{
    UBYTE g[20];
    int c;
    for (c = 0; c < ncell; c++)
        g[c] = pr_col[c / 4][c % 4];
    json_bytes("cells", g, ncell);
}

static void random_cells(int np, int ncell)
{
    int c;
    UBYTE mask = (UBYTE)((1 << np) - 1);
    for (c = 0; c < ncell; c++)
        cellcol[c] = (UBYTE)(amt_rand() >> 13) & mask;
    /* every single plane bit somewhere, and colour 0 */
    for (c = 0; c < np && c + 1 < ncell; c++)
        cellcol[c + 1] = (UBYTE)(1 << c);
    cellcol[0] = 0;
    cellcol[ncell - 1] = mask;
}

/* one mode test: resolution, planes, extra BPLCON0 bits */
static void mode_case(const char *id, int res, int np, UWORD con0x, const char *what, int want_shot)
{
    int rb, ok, bad = -1, s = -1, err;
    UBYTE mask = (UBYTE)((1 << np) - 1);
    reset_scene();
    sc.con0 = (UWORD)(res_con0(res, np) | con0x);
    std_ddf(res);
    sc.flags |= SF_PAL32 | (aga ? SF_PAL256 : 0);
    random_cells(np, 20);
    rb = fill_cells(np, 1 << res, 20, cellcol);
    sc.mod1 = sc.mod2 = (WORD)-rb;
    cell_probes(res, 20);
    apply();
    scan_colors(np);
    ok = check_cells(20, mask, &bad);
    if (want_shot)
        s = shot(id);
    display_off();
    amt_case_open(id, ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_bytes("cells", cellcol, 20);
        json_obj_end();
        json_obj("got");
        got_cells(20);
        json_int("first_bad_cell", bad);
        json_arr("clxdat_first_bad_group");
        for (err = 0; err < np; err++)
            json_hexn(NULL, pr_clx[bad / 4][err], 4);
        json_arr_end();
        json_obj_end();
        json_str("why", what);
    }
    json_obj("in");
    json_str("res", res_name[res]);
    json_int("planes", np);
    json_int("cell_lores_px", 16);
    json_hexn("probe_h0", origin[res], 3);
    json_str("probe", "sprite data $0180 (pixels 7-8) at origin+16*cell, lines from $48");
    json_int("probe_step", pstep);
    json_obj_end();
    json_scene();
    if (want_shot)
        json_shot(s);
    json_scan_ctx();
    amt_case_close();
}

static void mode_tests(void)
{
    static const char *const lo[9] = { 0, "mode_lores_1bpl", "mode_lores_2bpl", "mode_lores_3bpl",
                                       "mode_lores_4bpl", "mode_lores_5bpl", "mode_lores_6bpl",
                                       "mode_lores_7bpl", "mode_lores_8bpl" };
    static const char *const hi[5] = { 0, "mode_hires_1bpl", "mode_hires_2bpl", "mode_hires_3bpl",
                                       "mode_hires_4bpl" };
    int n, maxlo = aga ? 8 : 6;
    const char *w = "each 16-pixel cell must show the colour index of its bitplane words (planes "
                    "1..n from BPL1PT..BPLnPT, standard DDF fetch, negative modulo repeating the "
                    "row) (HRM 3: playfields; collisions HRM 7 read the index)";
    for (n = 1; n <= 8; n++) {
        if (n > maxlo) {
            amt_case(lo[n], AMT_ST_SKIP, "more than 6 bitplanes needs AGA (BPU3 in BPLCON0)");
            continue;
        }
        mode_case(lo[n], 0, n, 0, w, n == 5 || n == 6 || n == 8);
    }
    for (n = 1; n <= 4; n++)
        mode_case(hi[n], 1, n, 0, w, n == 4);
    if (ecsd && ecsa) {
        mode_case("mode_shres_1bpl", 2, 1, 0, w, 0);
        mode_case("mode_shres_2bpl", 2, 2, 0, w, 1);
    } else {
        amt_case("mode_shres_1bpl", AMT_ST_SKIP, "superhires (BPLCON0 SHRES) needs ECS Agnus and Denise");
        amt_case("mode_shres_2bpl", AMT_ST_SKIP, "superhires (BPLCON0 SHRES) needs ECS Agnus and Denise");
    }
    mode_case("mode_ham6_raw", 0, 6, 0x0800, "HAM6 (BPLCON0 HAM, 6 planes): the raw bitplane index "
              "seen by the collision logic must still be the fetched data (HRM 3: HAM; 7: collisions)", 1);
    mode_case("mode_dpf_raw", 0, 6, 0x0400, "dual playfield (BPLCON0 DBLPF, 6 planes): odd planes "
              "form PF1, even PF2; the raw index per plane is unchanged (HRM 3: dual playfield)", 1);
    if (aga)
        mode_case("mode_ham8_raw", 0, 8, 0x0800, "HAM8 (AGA, 8 planes + HAM): raw index per plane "
                  "(AGA documentation: HAM8)", 1);
    else
        amt_case("mode_ham8_raw", AMT_ST_SKIP, "HAM8 needs AGA");
}

/* ------------------------------------------------------------ sprite lists */
static UWORD *sw;
static void spr_begin(int s) { sw = spr_list[s]; }
static void spr_ent(UWORD vstart, UWORD vstop, UWORD h, UWORD ctlx, UWORD data, UWORD datb)
{
    UWORD n = (UWORD)(vstop - vstart);
    *sw++ = (UWORD)((vstart << 8) | ((h >> 1) & 0xff));
    *sw++ = (UWORD)((vstop << 8) | (h & 1) | ctlx | ((vstart & 0x100) ? 4 : 0) | ((vstop & 0x100) ? 2 : 0));
    while (n--) {
        *sw++ = data;
        *sw++ = datb;
    }
}
static void spr_end(void) { *sw++ = 0; *sw++ = 0; }
static void spr_all_empty(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        spr_begin(i);
        spr_end();
    }
}

/* whole frame (custom sprite lists): see clean_clx */
static UWORD frame_clx(UWORD clx, UWORD clx2, int flags) { return clean_clx(clx, clx2, flags); }

/* ------------------------------------------------------------ collision model */
/* HRM 7: a playfield group matches when every enabled plane of the group
 * equals its match bit (no enabled plane: always matches). */
static int grp_match(UBYTE col, UWORD en, UWORD mv, UBYTE grp)
{
    return ((col ^ mv) & en & grp) == 0;
}

static void clx_unpack(UWORD clx, UWORD clx2, UWORD *en, UWORD *mv)
{
    *en = (UWORD)((clx >> 6) & 0x3f);
    *mv = (UWORD)(clx & 0x3f);
    if (aga) {
        *en |= (UWORD)(clx2 & 0xc0);
        *mv |= (UWORD)((clx2 & 3) << 6);
    }
}

/* ---- PF1-PF2 (bit 0), whole frame */
static void clx_pfpf_tests(void)
{
    int np = aga ? 8 : 6, n = AMT_QUICK() ? 8 : 32, i, c, bad = -1, rb;
    UWORD clx[32], clx2[32], got[32], exp[32];
    reset_scene();
    sc.con0 = res_con0(0, np);
    sc.flags |= SF_CUSTSPR;
    spr_all_empty();
    random_cells(np, 20);
    rb = fill_cells(np, 1, 20, cellcol);
    sc.mod1 = sc.mod2 = (WORD)-rb;
    for (i = 0; i < n; i++) {
        ULONG r = amt_rand();
        clx[i] = (UWORD)(r & 0x0fff);
        clx2[i] = (UWORD)((r >> 16) & 0xc3);
        if (i == 0)
            clx[i] = 0, clx2[i] = 0;           /* nothing enabled: always */
        if (i == 1)
            clx[i] = 0x0fff, clx2[i] = 0xc3;   /* all planes, all ones */
    }
    apply();
    for (i = 0; i < n; i++) {
        UWORD en, mv;
        int e = 0;
        clx_unpack(clx[i], clx2[i], &en, &mv);
        for (c = 0; c < 20; c++)
            if (grp_match(cellcol[c], en, mv, 0x55) && grp_match(cellcol[c], en, mv, 0xaa))
                e = 1;
        exp[i] = (UWORD)e;
        got[i] = frame_clx(clx[i], clx2[i], 0) & 1;
        if (got[i] != exp[i] && bad < 0)
            bad = i;
    }
    display_off();
    amt_case_open("clx_pf1_pf2", bad < 0 ? AMT_ST_PASS : AMT_ST_FAIL);
    if (bad >= 0) {
        json_obj("expect");
        json_int("bit0", exp[bad]);
        json_obj_end();
        json_obj("got");
        json_int("bit0", got[bad]);
        json_obj_end();
        json_obj("in");
        json_hexn("clxcon", clx[bad], 4);
        if (aga)
            json_hexn("clxcon2", clx2[bad], 4);
        json_bytes("row_cells", cellcol, 20);
        json_int("combo", bad);
        json_obj_end();
        json_str("why", "CLXDAT bit 0 (even to odd bitplanes) must be set iff some displayed pixel "
                 "matches CLXCON on the enabled odd planes AND on the enabled even planes (a group "
                 "with no enabled plane always matches) (HRM 7: CLXCON/CLXDAT; AGA: CLXCON2)");
    }
    json_int("combos", n);
    json_scene();
    amt_case_close();
}

/* ---- sprite to playfield: per clean frame a random CLXCON/CLXCON2 and 4
 * one-line sprites on random cells (same line) */
static void clx_sprpf_case(const char *id, int dpf, int combos)
{
    int np = aga ? 8 : 6, f, s, badf = -1, bads = 0;
    int ok_ind = 1, ok_cpl = 1;          /* single playfield: which model fits every combo */
    UWORD bexp = 0, bgot = 0, bclx = 0, bclx2 = 0;
    UBYTE bcol = 0, cell[4];
    reset_scene();
    sc.con0 = (UWORD)(res_con0(0, np) | (dpf ? 0x0400 : 0));
    random_cells(np, 20);
    sc.mod1 = sc.mod2 = (WORD)-fill_cells(np, 1, 20, cellcol);
    reset_probes();
    pr_n = 1;
    apply();
    for (f = 0; f < combos && badf < 0; f++) {
        ULONG r = amt_rand();
        UWORD clx = (UWORD)(r & 0x0fff), clx2 = (UWORD)((r >> 16) & 0xc3), en, mv, e = 0, ec = 0, g;
        static const UBYTE bit[4][4] = { { 0, 9, 10, 11 }, { 0, 0, 12, 13 }, { 0, 0, 0, 14 }, { 0 } };
        int a, b;
        for (s = 0; s < 4; s++) {
            cell[s] = (UBYTE)((amt_rand() >> 11) % 20);
            pr[0][s].h = (UWORD)(origin[0] + 16 * cell[s]);
            pr[0][s].data = 0x0180;
        }
        build_group(0, 0, 4);
        g = clean_clx(clx, clx2, 0) & 0x7ffe;
        clx_unpack(clx, clx2, &en, &mv);
        for (s = 0; s < 4; s++) {
            UBYTE col = cellcol[cell[s]];
            int m1 = grp_match(col, en, mv, 0x55), m2 = grp_match(col, en, mv, 0xaa);
            if (m1)
                e |= (UWORD)(2 << s);
            if (m2)
                e |= (UWORD)(0x20 << s);
            if (m1 && m2)                   /* coupled variant (single playfield) */
                ec |= (UWORD)(2 << s);
            if (m2)
                ec |= (UWORD)(0x20 << s);
        }
        for (a = 0; a < 4; a++)
            for (b = a + 1; b < 4; b++)
                if (cell[a] == cell[b]) {
                    e |= (UWORD)(1 << bit[a][b]);
                    ec |= (UWORD)(1 << bit[a][b]);
                }
        if (g != e)
            ok_ind = 0;
        if (g != ec)
            ok_cpl = 0;
        if (dpf ? g != e : !(ok_ind || ok_cpl)) {
            badf = f;
            bexp = e;
            bgot = g;
            bclx = clx;
            bclx2 = clx2;
            for (s = 0; s < 4; s++)
                if (((e ^ g) >> (1 + s)) & 0x11)
                    break;
            bads = s & 3;
            bcol = cellcol[cell[bads]];
        }
    }
    display_off();
    amt_case_open(id, badf < 0 ? AMT_ST_PASS : AMT_ST_FAIL);
    if (badf >= 0) {
        json_obj("expect");
        json_hexn("clxdat", bexp, 4);
        if (!dpf)
            json_str("alternative", "coupled model: odd bit only where the even group matches too");
        json_obj_end();
        json_obj("got");
        json_hexn("clxdat", bgot, 4);
        json_obj_end();
        json_obj("in");
        json_hexn("clxcon", bclx, 4);
        if (aga)
            json_hexn("clxcon2", bclx2, 4);
        json_int("combo", badf);
        json_int("probe_line", PY(0));
        json_int("first_bad_sprite", bads * 2);
        json_hexn("colour_under_it", bcol, 2);
        json_bytes("sprite_cells", cell, 4);
        json_bytes("row_cells", cellcol, 20);
        json_str("mask", "bit 0 (non-local) and bit 15 ignored; bits 9-14 set where two probes share a cell");
        json_obj_end();
        json_str("why", dpf ? "dual playfield: CLXDAT bit 1+n/5+n (odd/even planes to sprite 2n) must be "
                 "set iff the pixels under sprite 2n match CLXCON on the enabled odd/even planes "
                 "(HRM 7: collisions; AGA CLXCON2 for planes 7-8)"
                 : "single playfield: the even-plane bits follow the match rule; the odd-plane bits "
                 "must follow one of the two models (independent, or needing the even match too) "
                 "consistently over all combinations (HRM 7 does not specify; see "
                 "clx_spf_odd_independent)");
    }
    json_int("combos", combos);
    if (!dpf) {
        json_obj("ctx");
        json_str("undef_coupling", ok_ind && ok_cpl ? "both_fit" : ok_ind ? "independent"
                 : ok_cpl ? "coupled" : "neither");
        json_obj_end();
    }
    json_scene();
    json_scan_ctx();
    amt_case_close();
}

/* single playfield: odd group matches, even group does not */
static void clx_spf_odd(void)
{
    UWORD r[2][2];
    int m, ok;
    static const UBYTE col2[2] = { 1, 2 };
    for (m = 0; m < 2; m++) {
        reset_scene();
        sc.con0 = (UWORD)(res_con0(0, 2) | (m ? 0x0400 : 0));
        lores_row(2, 2, col2);            /* cells alternate colour 1, 2 */
        reset_probes();
        pr_n = 1;
        pr[0][0].h = origin[0];
        pr[0][0].data = 0x0180;
        pr[0][1].h = (UWORD)(origin[0] + 16);
        pr[0][1].data = 0x0180;
        apply();
        build_group(0, 0, 4);
        {
            UWORD v = clean_clx(0x00c3, 0, 0);   /* planes 1+2 enabled, match 1,1 */
            r[m][0] = v & 0x0022;                /* sprite 0: odd / even */
            r[m][1] = v & 0x0044;                /* sprite 2 */
        }
    }
    display_off();
    /* single playfield: the HRM does not say whether the odd-plane bit
     * needs the even group to match too (WinUAE couples them, on purpose).
     * Both are accepted; which one was seen goes to ctx.undef_coupling. DPF
     * stays strict. */
    ok = (r[0][0] == 0x0002 || r[0][0] == 0x0000) && r[0][1] == 0x0040 &&
         r[1][0] == 0x0002 && r[1][1] == 0x0040;
    amt_case_open("clx_spf_odd_independent", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_str("spf_spr0", "0002 (independent) or 0000 (coupled)");
        json_hexn("spf_spr2", 0x0040, 4);
        json_hexn("dpf_spr0", 0x0002, 4);
        json_hexn("dpf_spr2", 0x0040, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("spf_spr0", r[0][0], 4);
        json_hexn("spf_spr2", r[0][1], 4);
        json_hexn("dpf_spr0", r[1][0], 4);
        json_hexn("dpf_spr2", r[1][1], 4);
        json_obj_end();
        json_obj("in");
        json_hexn("clxcon", 0x00c3, 4);
        json_str("cells", "sprite 0 over colour 1 (plane 1 set, plane 2 clear); sprite 2 over colour 2");
        json_obj_end();
        json_str("why", "dual playfield: CLXDAT bit 1 (odd planes to sprite 0) depends only on the odd "
                 "planes, bit 6 only on the even planes (HRM 7: collision detection). Single "
                 "playfield: bit 6 (even match) must be set; bit 1 with only the odd group matching "
                 "is not specified by the HRM, both values are accepted");
    }
    json_str("modes", "spf_* = single playfield, dpf_* = BPLCON0 DBLPF");
    json_obj("ctx");
    json_str("undef_coupling", r[0][0] == 0x0002 ? "independent" : r[0][0] == 0 ? "coupled" : "other");
    json_str("check_on_real_hardware", "single-playfield odd/even coupling of CLXDAT bits 1-4");
    json_obj_end();
    amt_case_close();
}

/* ---- sprite to sprite, whole frame, all ENSP combinations */
static void clx_sprspr_tests(void)
{
    int nl = AMT_QUICK() ? 3 : 8, l, e, s, badl = -1, bade = 0;
    UWORD bexp = 0, bgot = 0;
    UBYTE slot[8], bslot[8];
    for (l = 0; l < nl && badl < 0; l++) {
        static const UBYTE z[1] = { 0 };
        reset_scene();
        sc.con0 = res_con0(0, 1);
        lores_row(1, 1, z);
        sc.flags |= SF_CUSTSPR | SF_ODDSPR;
        for (s = 0; s < 8; s++) {
            slot[s] = (UBYTE)((amt_rand() >> 9) % 5);
            if (l == 0)
                slot[s] = (UBYTE)(s & 1 ? 4 : 0);       /* even sprites stacked */
            if (l == 1)
                slot[s] = (UBYTE)(s & 1 ? 1 : 4);       /* odd sprites stacked */
            spr_begin(s);
            if (slot[s] < 4)
                spr_ent(0x80, 0x84, (UWORD)(0xa0 + 40 * slot[s]), 0, 0xffff, 0);
            spr_end();
        }
        apply();
        for (e = 0; e < 16 && badl < 0; e++) {
            UWORD clx = (UWORD)(e << 12), x = 0, g;
            int a, b, k;
            UBYTE pres[4][4];
            for (k = 0; k < 4; k++)
                for (a = 0; a < 4; a++)
                    pres[a][k] = (slot[2 * a] == k) || (((e >> a) & 1) && slot[2 * a + 1] == k);
            for (a = 0; a < 4; a++)
                for (b = a + 1; b < 4; b++)
                    for (k = 0; k < 4; k++)
                        if (pres[a][k] && pres[b][k]) {
                            static const UBYTE bit[4][4] = { { 0, 9, 10, 11 }, { 0, 0, 12, 13 },
                                                             { 0, 0, 0, 14 }, { 0 } };
                            x |= (UWORD)(1 << bit[a][b]);
                        }
            g = frame_clx(clx, 0, 0) & 0x7e00;
            if (g != x) {
                badl = l;
                bade = e;
                bexp = x;
                bgot = g;
                memcpy(bslot, slot, 8);
            }
        }
    }
    display_off();
    amt_case_open("clx_spr_spr", badl < 0 ? AMT_ST_PASS : AMT_ST_FAIL);
    if (badl >= 0) {
        json_obj("expect");
        json_hexn("clxdat_9_14", bexp, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("clxdat_9_14", bgot, 4);
        json_obj_end();
        json_obj("in");
        json_hexn("clxcon", (UWORD)(bade << 12), 4);
        json_bytes("sprite_slot", bslot, 8);
        json_str("slots", "slot k = H $A0+40k, lines $80-$83, data $FFFF; slot 4 = sprite unused");
        json_int("layout", badl);
        json_obj_end();
        json_str("why", "CLXDAT bits 9-14 flag overlaps between sprite groups 0/2/4/6, where odd "
                 "sprite 2n+1 joins group 2n only when its CLXCON ENSP bit (12-15) is set "
                 "(HRM 7: CLXDAT table)");
    }
    json_int("layouts", nl);
    amt_case_close();
}

/* ---- CLXDAT clears when read */
static void clx_clear_test(void)
{
    static const UBYTE z[1] = { 0 };
    UWORD a, b;
    reset_scene();
    sc.con0 = res_con0(0, 1);
    lores_row(1, 1, z);
    sc.flags |= SF_CUSTSPR;
    spr_all_empty();
    spr_begin(0);
    spr_ent(0x80, 0x84, 0xa0, 0, 0xffff, 0);
    spr_end();
    spr_begin(2);
    spr_ent(0x80, 0x84, 0xa0, 0, 0xffff, 0);
    spr_end();
    apply();
    (void)frame_clx(0, 0, 0);
    /* at the next VBL: the frame's collisions, then (back to back, in the
     * blank lines, long before the sprites at $80) nothing */
    wait_vbl();
    amt_set_sr(0x2700);
    a = CU(CLXDAT) & 0x7fff;
    b = CU(CLXDAT) & 0x7fff;
    amt_set_sr(0x2000);
    display_off();
    amt_case_open("clx_read_clears", ((a & 0x0200) && !b) ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!((a & 0x0200) && !b)) {
        json_obj("expect");
        json_str("first", "bit 9 set (sprite 0 overlaps sprite 2)");
        json_hexn("second", 0, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("first", a, 4);
        json_hexn("second", b, 4);
        json_obj_end();
        json_str("why", "CLXDAT latches collisions until it is read and is cleared by the read; the "
                 "second read, right after the first in the vertical blank, must be 0 (bit 15 ignored) "
                 "(HRM 7: CLXDAT)");
    }
    amt_case_close();
}

/* ---- AGA: a CLXCON write clears CLXCON2 */
static void clx_clxcon2_reset(void)
{
    static const UBYTE col1[1] = { 0 };
    UWORD a, b;
    int ok;
    if (!aga) {
        amt_case("clx_aga_clxcon2_reset", AMT_ST_SKIP, "CLXCON2 is AGA only");
        return;
    }
    reset_scene();
    sc.con0 = res_con0(0, 8);
    lores_row(8, 1, col1);                 /* all planes 0 */
    sc.flags |= SF_CUSTSPR;
    spr_all_empty();
    spr_begin(0);
    spr_ent(0x80, 0x84, (UWORD)(origin[0] + 64), 0, 0xffff, 0);
    spr_end();
    apply();
    a = frame_clx(0x0000, 0x0041, 0) & 0x0002;            /* plane 7 must be 1: no hit */
    b = frame_clx(0x0000, 0x0041, FC_REVERSE) & 0x0002;   /* CLXCON2 cleared: hit */
    display_off();
    ok = a == 0 && b == 0x0002;
    amt_case_open("clx_aga_clxcon2_reset", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_int("bit1_clxcon2_last", 0);
        json_int("bit1_clxcon_last", 1);
        json_obj_end();
        json_obj("got");
        json_int("bit1_clxcon2_last", a != 0);
        json_int("bit1_clxcon_last", b != 0);
        json_obj_end();
        json_obj("in");
        json_hexn("clxcon", 0, 4);
        json_hexn("clxcon2", 0x41, 4);
        json_str("playfield", "8 planes all 0 under sprite 0");
        json_obj_end();
        json_str("why", "AGA: CLXCON2 ENBP7/MVBP7=1 makes plane 7 (0 here) block the odd-plane match; "
                 "a later CLXCON write resets CLXCON2, so the match (and bit 1) comes back "
                 "(AGA documentation: CLXCON2)");
    }
    amt_case_close();
}

/* ---- attached sprites collide like unattached ones */
static void clx_attached(void)
{
    static const UBYTE col1[1] = { 1 };
    UWORD a, b;
    int ok;
    reset_scene();
    sc.con0 = res_con0(0, 1);
    lores_row(1, 1, col1);
    sc.flags |= SF_CUSTSPR | SF_ODDSPR;
    spr_all_empty();
    spr_begin(0);
    spr_ent(0x80, 0x84, 0xa0, 0, 0, 0);
    spr_end();
    spr_begin(1);
    spr_ent(0x80, 0x84, 0xa0, 0x80, 0xffff, 0);   /* ATT */
    spr_end();
    apply();
    a = frame_clx(0x0041, 0, 0) & 0x0002;
    b = frame_clx(0x1041, 0, 0) & 0x0002;
    display_off();
    ok = a == 0 && b == 2;
    amt_case_open("clx_attached_ensp", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_int("bit1_ensp1_off", 0);
        json_int("bit1_ensp1_on", 1);
        json_obj_end();
        json_obj("got");
        json_int("bit1_ensp1_off", a != 0);
        json_int("bit1_ensp1_on", b != 0);
        json_obj_end();
        json_obj("in");
        json_str("sprites", "sprite 0 data 0; sprite 1 attached (CTL bit 7) data $FFFF; playfield plane 1 = 1");
        json_hexn("clxcon_off", 0x0041, 4);
        json_hexn("clxcon_on", 0x1041, 4);
        json_obj_end();
        json_str("why", "odd sprite 1 takes part in group 0 collisions only with CLXCON ENSP1; the "
                 "ATTACH bit changes colours, not the collision rules (HRM 4/7)");
    }
    amt_case_close();
}

/* ---- manual mode sprite (no sprite DMA) */
static void spr_manual(void)
{
    static const UBYTE col2[2] = { 1, 0 };
    UWORD a, b, c, d;
    int ok;
    reset_scene();
    sc.con0 = res_con0(0, 1);
    lores_row(1, 2, col2);                 /* cells alternate 1,0 */
    sc.flags |= SF_NOSPR | SF_CUSTSPR;
    spr_all_empty();
    apply();
    /* sprite 0 over a colour-1 cell (cell 4) */
    CU(SPR0CTL) = 0;
    CU(SPR0POS) = (UWORD)(((origin[0] + 64) >> 1) & 0xff);
    CU(SPR0CTL) = (UWORD)((origin[0] + 64) & 1);
    CU(SPR0DATB) = 0;
    CU(SPR0DATA) = 0x0ff0;                 /* arms */
    a = frame_clx(0x0041, 0, 0) & 0x0002;
    /* move over a colour-0 cell (cell 5): no hit */
    CU(SPR0POS) = (UWORD)(((origin[0] + 80) >> 1) & 0xff);
    CU(SPR0CTL) = (UWORD)((origin[0] + 80) & 1);   /* disarms */
    CU(SPR0DATA) = 0x0ff0;                         /* re-arms */
    b = frame_clx(0x0041, 0, 0) & 0x0002;
    /* back over cell 4, then CTL write disarms */
    CU(SPR0POS) = (UWORD)(((origin[0] + 64) >> 1) & 0xff);
    CU(SPR0CTL) = (UWORD)((origin[0] + 64) & 1);
    c = frame_clx(0x0041, 0, 0) & 0x0002;
    CU(SPR0DATA) = 0x0ff0;
    d = frame_clx(0x0041, 0, 0) & 0x0002;
    CU(SPR0CTL) = 0;
    display_off();
    ok = a && !b && !c && d;
    amt_case_open("spr_manual_mode", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_int("armed_on_colour1", 1);
        json_int("armed_on_colour0", 0);
        json_int("disarmed_by_ctl", 0);
        json_int("rearmed_by_data", 1);
        json_obj_end();
        json_obj("got");
        json_int("armed_on_colour1", a != 0);
        json_int("armed_on_colour0", b != 0);
        json_int("disarmed_by_ctl", c != 0);
        json_int("rearmed_by_data", d != 0);
        json_obj_end();
        json_obj("in");
        json_str("sequence", "sprite DMA off; CPU writes SPR0CTL, SPR0POS, SPR0DATB, SPR0DATA=$0FF0; "
                 "cells of 16 px alternate colour 1/0; CLXCON $0041");
        json_obj_end();
        json_str("why", "manual mode: writing SPRxDATA arms the sprite, which then shows its data on "
                 "every line at its H position; writing SPRxCTL disarms it (HRM 4: manual mode)");
    }
    json_str("note", "all reads are of whole, static frames (timing-independent); Amiberry with "
             "cpu_compatible=false loses manual-mode sprites entirely, so this case differs "
             "between its cores: that is the emulator bug, not test timing");
    amt_case_close();
}

/* ---- sprite VSTOP is exclusive, against the vertical display window */
static void spr_vertical(void)
{
    static const UBYTE col1[1] = { 1 };
    static const UWORD st[4] = { DV0 - 3, DV0 - 3, DV1, DV1 - 1 };
    static const UWORD sp[4] = { DV0, DV0 + 1, DV1 + 2, DV1 + 2 };
    static const UBYTE ex[4] = { 0, 1, 0, 1 };
    UBYTE got[4];
    int i, ok = 1;
    for (i = 0; i < 4; i++) {
        reset_scene();
        sc.con0 = res_con0(0, 1);
        lores_row(1, 1, col1);
        sc.flags |= SF_CUSTSPR;
        spr_all_empty();
        spr_begin(0);
        spr_ent(st[i], sp[i], 0xa0, 0, 0xffff, 0);
        spr_end();
        apply();
        got[i] = (frame_clx(0x0041, 0, 0) & 2) ? 1 : 0;
        if (got[i] != ex[i])
            ok = 0;
    }
    display_off();
    amt_case_open("spr_vstart_vstop", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_bytes("hit", ex, 4);
        json_obj_end();
        json_obj("got");
        json_bytes("hit", got, 4);
        json_obj_end();
        json_obj("in");
        json_hexn("diw_v", (DV0 << 8) | DV1, 4);
        json_str("sprites", "VSTART/VSTOP = $3D/$40, $3D/$41, $F0/$F2, $EF/$F2 over a plane-1=1 playfield");
        json_obj_end();
        json_str("why", "a sprite is shown on lines VSTART..VSTOP-1 (VSTOP is the first line not "
                 "shown), and the playfield on DIW lines VSTART..VSTOP-1; they collide only where "
                 "both are shown (HRM 3: display window; 4: sprite position)");
    }
    amt_case_close();
}

/* ------------------------------------------------------------ edges */
/* first probe position (of n one-pixel probes from hfrom) that sees plane
 * `pl` set; -1 if none. last = last hit. */
static void edge_measure(UWORD hfrom, int n, int pl, int np, int *first, int *last, UBYTE fine)
{
    int j;
    reset_probes();
    for (j = 0; j < n; j++) {
        struct probe *p = &pr[j / 4][j % 4];
        p->h = (UWORD)(hfrom + (fine ? (j >> 2) : j));
        p->fine = fine ? (UBYTE)(j & 3) : 0;
        p->data = 0x8000;
    }
    pr_n = (n + 3) / 4;
    apply();
    scan_mask((UBYTE)(1 << pl));
    (void)np;
    *first = *last = -1;
    for (j = 0; j < n; j++)
        if ((pr_col[j / 4][j % 4] >> pl) & 1) {
            if (*first < 0)
                *first = j;
            *last = j;
        }
}

static void fail_edges(const char *id, const char *what, const UWORD *par, const int *exp,
                       const int *got, int n, const char *why)
{
    int i;
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_arr("pos");
    for (i = 0; i < n; i++)
        json_int(NULL, exp[i]);
    json_arr_end();
    json_obj_end();
    json_obj("got");
    json_arr("pos");
    for (i = 0; i < n; i++)
        json_int(NULL, got[i]);
    json_arr_end();
    json_obj_end();
    json_obj("in");
    json_str("param", what);
    json_arr("values");
    for (i = 0; i < n; i++)
        json_hexn(NULL, par[i], 4);
    json_arr_end();
    json_obj_end();
    json_str("why", why);
    json_scene();
    amt_case_close();
}

/* horizontal display window start/stop, relative to the standard window */
static void diw_h_tests(void)
{
    static const UBYTE one[1] = { 1 };
    static const UWORD hs[5] = { 0x81, 0x82, 0x85, 0x91, 0xc0 };
    static const UWORD he[5] = { 0xc1, 0xc0, 0xb9, 0xa1, 0x60 };
    int got[5], exp[5], i, f, l, ref = 0, ok = 1;
    /* start: first visible pixel moves with DIWSTRT H */
    for (i = 0; i < 5; i++) {
        reset_scene();
        sc.con0 = res_con0(0, 1);
        lores_row(1, 1, one);
        sc.diwstrt = (UWORD)((DV0 << 8) | hs[i]);
        edge_measure((UWORD)(hs[i] - 12), 24, 0, 1, &f, &l, 0);
        got[i] = f < 0 ? -1 : (int)(hs[i] - 12 + f);
        if (i == 0)
            ref = got[0];
        exp[i] = ref < 0 ? -2 : ref + (hs[i] - 0x81);
        if (got[i] != exp[i])
            ok = 0;
    }
    display_off();
    if (ok)
        amt_case("diw_hstart", AMT_ST_PASS, 0);
    else
        fail_edges("diw_hstart", "DIWSTRT H", hs, exp, got, 5,
                   "the first sprite H that collides with an all-ones playfield must move one for "
                   "one with DIWSTRT HSTART (lores resolution) (HRM 3: display window)");
    /* stop: last visible pixel; DIWSTOP H8 is implied 1 (HRM) */
    ok = 1;
    for (i = 0; i < 5; i++) {
        UWORD stop = (UWORD)(0x100 | he[i]);
        reset_scene();
        sc.con0 = res_con0(0, 1);
        lores_row(1, 1, one);
        sc.diwstop = (UWORD)(((DV1 & 0xff) << 8) | he[i]);
        edge_measure((UWORD)(stop - 12), 24, 0, 1, &f, &l, 0);
        got[i] = l < 0 ? -1 : (int)(stop - 12 + l);
        if (i == 0)
            ref = got[0];
        exp[i] = ref < 0 ? -2 : ref + (he[i] - 0xc1);
        if (got[i] != exp[i])
            ok = 0;
    }
    display_off();
    if (ok)
        amt_case("diw_hstop", AMT_ST_PASS, 0);
    else
        fail_edges("diw_hstop", "DIWSTOP H (H8 implied 1)", he, exp, got, 5,
                   "the last sprite H that collides with an all-ones playfield must move one for "
                   "one with DIWSTOP HSTOP; OCS implies HSTOP bit 8 = 1 (HRM 3: display window)");
}

/* ECS DIWHIGH: explicit H8 */
static void diwhigh_test(void)
{
    static const UBYTE one[1] = { 1 };
    int f, l, ref, got, ok;
    UWORD par[1] = { 0x2000 };
    if (!ecsa || !ecsd) {
        amt_case("diwhigh_hstop_h8", AMT_ST_SKIP, "DIWHIGH needs ECS Agnus and ECS Denise");
        return;
    }
    reset_scene();
    sc.con0 = res_con0(0, 1);
    lores_row(1, 1, one);
    edge_measure(0x1c1 - 12, 24, 0, 1, &f, &l, 0);
    ref = l < 0 ? -1 : 0x1c1 - 12 + l;
    /* DIWSTOP H = $F1 with H8 = 0 from DIWHIGH: stop 208 pixels earlier */
    reset_scene();
    sc.con0 = res_con0(0, 1);
    lores_row(1, 1, one);
    sc.diwstop = (UWORD)(((DV1 & 0xff) << 8) | 0xf1);
    sc.diwhigh = 0x0000;          /* stop H8=0, start H8=0, V8 bits 0 */
    sc.flags |= SF_DIWHIGH;
    edge_measure(0xf1 - 12, 24, 0, 1, &f, &l, 0);
    got = l < 0 ? -1 : 0xf1 - 12 + l;
    display_off();
    ok = ref >= 0 && got == ref - (0x1c1 - 0xf1);
    if (ok)
        amt_case("diwhigh_hstop_h8", AMT_ST_PASS, 0);
    else {
        int e = ref - (0x1c1 - 0xf1);
        fail_edges("diwhigh_hstop_h8", "DIWHIGH (DIWSTOP $F0F1)", par, &e, &got, 1,
                   "with DIWHIGH written, DIWSTOP H8 comes from DIWHIGH bit 13 (0 here), so the "
                   "window stops at H $0F1 instead of $1F1 (HRM appendix C: DIWHIGH)");
    }
}

/* vertical window: lines DV-1/DV and VSTOP-1/VSTOP, one probe each; and a
 * VSTOP below $80, whose implied bit 8 is 1 (HRM: V8 = NOT V7) */
static void diw_v_test(void)
{
    static const UBYTE one[1] = { 1 };
    UWORD d0 = (UWORD)(PY(2) + 1), d1 = (UWORD)(PY(19) + 1);
    UBYTE got[6], ex[6] = { 0, 1, 1, 0, 1, 1 };
    int ok = 1, i, k;
    for (k = 0; k < 2; k++) {
        reset_scene();
        sc.con0 = res_con0(0, 1);
        lores_row(1, 1, one);
        sc.diwstrt = (UWORD)((d0 << 8) | 0x81);
        /* k=1: VSTOP $5B means line $15B: lines $5A/$5B still shown */
        sc.diwstop = (UWORD)(((k ? PY(6) + 1 : d1) << 8) | 0xc1);
        reset_probes();
        pr_n = 20;
        if (!k) {
            pr[2][0].h = 0xa0; pr[2][0].dy = 0; pr[2][0].data = 0xffff;   /* d0-1 */
            pr[2][1].h = 0xc0; pr[2][1].dy = 1; pr[2][1].data = 0xffff;   /* d0 */
            pr[19][0].h = 0xa0; pr[19][0].dy = 0; pr[19][0].data = 0xffff; /* d1-1 */
            pr[19][1].h = 0xc0; pr[19][1].dy = 1; pr[19][1].data = 0xffff; /* d1 */
        } else {
            pr[6][0].h = 0xa0; pr[6][0].dy = 0; pr[6][0].data = 0xffff;
            pr[6][1].h = 0xc0; pr[6][1].dy = 1; pr[6][1].data = 0xffff;
        }
        apply();
        scan_colors(1);
        if (!k) {
            got[0] = pr_col[2][0] & 1;
            got[1] = pr_col[2][1] & 1;
            got[2] = pr_col[19][0] & 1;
            got[3] = pr_col[19][1] & 1;
        } else {
            got[4] = pr_col[6][0] & 1;
            got[5] = pr_col[6][1] & 1;
        }
    }
    display_off();
    for (i = 0; i < 6; i++)
        if (got[i] != ex[i])
            ok = 0;
    amt_case_open("diw_vstart_vstop", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_bytes("hit", ex, 6);
        json_obj_end();
        json_obj("got");
        json_bytes("hit", got, 6);
        json_obj_end();
        json_obj("in");
        json_arr("lines");
        json_int(NULL, d0 - 1);
        json_int(NULL, d0);
        json_int(NULL, d1 - 1);
        json_int(NULL, d1);
        json_int(NULL, PY(6));
        json_int(NULL, PY(6) + 1);
        json_arr_end();
        json_hexn("diwstrt", (d0 << 8) | 0x81, 4);
        json_hexn("diwstop_a", (d1 << 8) | 0xc1, 4);
        json_hexn("diwstop_b", ((PY(6) + 1) << 8) | 0xc1, 4);
        json_obj_end();
        json_str("why", "the playfield is shown on lines DIW VSTART..VSTOP-1 only; DIWSTOP's line "
                 "bit 8 is the complement of bit 7, so VSTOP $5B is line $15B and lines $5A/$5B are "
                 "still inside the window (HRM 3: display window)");
    }
    amt_case_close();
}

/* ------------------------------------------------------------ fetch, modulo */
/* address-encoded planes from word AE_BASE: plane p word w = bit p of w */
#define AE_BASE 1024
static void ae_setup(int np)
{
    fill_ae(np, PL_BYTES / 2);
    ptr_off = AE_BASE;
}

/* probes: rows 3i and 3i+1 (dy 0/1), two columns per probe line */
static void ae_probes(int res, int k0, const int *cols, int nrows3)
{
    int i;
    UWORD wpx = (UWORD)(16 >> res);       /* lores px per word */
    reset_probes();
    pr_n = nrows3;
    for (i = 0; i < nrows3; i++) {
        int s;
        for (s = 0; s < 4; s++) {
            struct probe *p = &pr[i][s];
            int col = cols[s >> 1];
            p->h = (UWORD)(origin[res] + (k0 + col) * wpx + wpx / 2 - (res ? 1 : 2));
            p->dy = (UBYTE)(s & 1);
            p->data = res ? 0xc000 : 0xf000;
        }
    }
}

/* expected index at (row, col): odd planes from BPL1MOD, even from BPL2MOD */
static UBYTE ae_expect(int np, int W, int row, int col)
{
    long w1 = (long)row * (W + sc.mod1 / 2) + col, w2 = (long)row * (W + sc.mod2 / 2) + col;
    UBYTE m = (UBYTE)((1 << np) - 1);
    return (UBYTE)(((w1 & 0x55) | (w2 & 0xaa)) & m);
}

struct ddfcase {
    const char *id;
    UBYTE res;
    UBYTE strt, stop;
    BYTE m1, m2;
    UBYTE words;
};

static const struct ddfcase ddfc[] = {
    { "ddf_lores_38_d0", 0, 0x38, 0xd0, 0, 0, 20 },
    { "ddf_lores_40_d0", 0, 0x40, 0xd0, 0, 0, 19 },
    { "ddf_lores_38_c0", 0, 0x38, 0xc0, 0, 0, 18 },
    { "ddf_lores_30_d0", 0, 0x30, 0xd0, 0, 0, 21 },
    { "ddf_lores_48_a0", 0, 0x48, 0xa0, 0, 0, 12 },
    { "ddf_lores_38_d8", 0, 0x38, 0xd8, 0, 0, 21 },
    { "ddf_hires_3c_d4", 1, 0x3c, 0xd4, 0, 0, 40 },
    { "ddf_hires_44_d4", 1, 0x44, 0xd4, 0, 0, 38 },
    { "ddf_hires_3c_c4", 1, 0x3c, 0xc4, 0, 0, 36 },
    { "modulo_lores_odd_even", 0, 0x38, 0xd0, 8, -6, 20 },
    { "modulo_lores_negative", 0, 0x38, 0xd0, -40, -44, 20 },
    { "modulo_hires_odd_even", 1, 0x3c, 0xd4, 12, 2, 40 },
};

static void ddf_tests(void)
{
    unsigned n;
    for (n = 0; n < sizeof(ddfc) / sizeof(ddfc[0]); n++) {
        const struct ddfcase *d = &ddfc[n];
        int np = d->res ? 4 : 6, k0, cols[2], ok, bad = 0, sprmax;
        UBYTE exp[NPL * 4], got[NPL * 4];
        reset_scene();
        sc.con0 = res_con0(d->res, np);
        sc.ddfstrt = d->strt;
        sc.ddfstop = d->stop;
        sc.mod1 = d->m1;
        sc.mod2 = d->m2;
        sc.diwstrt = (UWORD)((DV0 << 8) | 0x81);
        ae_setup(np);
        /* word 0 of the fetch is k0 words right of the standard first word */
        k0 = d->res ? (d->strt - 0x3c) / 4 : (d->strt - 0x38) / 8;
        cols[0] = k0 < 0 ? -k0 + 1 : 1;
        cols[1] = d->words - 2;
        ae_probes(d->res, k0, cols, 4);
        /* sprites 6 (and 4) lose their DMA slots to an early fetch */
        sprmax = d->strt < 0x38 ? 2 : 4;
        {
            int i, s;
            for (i = 0; i < pr_n; i++)
                for (s = sprmax; s < 4; s++)
                    pr[i][s].data = 0;
            if (sprmax < 4)
                cols[1] = cols[0] + 2;
            for (i = 0; i < pr_n; i++)
                for (s = 0; s < sprmax; s++) {
                    UWORD wpx = (UWORD)(16 >> d->res);
                    int col = cols[s >> 1];
                    pr[i][s].h = (UWORD)(origin[d->res] + (k0 + col) * wpx + wpx / 2 - (d->res ? 1 : 2));
                }
        }
        apply();
        scan_colors(np);
        display_off();
        {
            int i, s, m = 0, okk = 1;
            for (i = 0; i < pr_n; i++)
                for (s = 0; s < 4; s++) {
                    int row = pstep * i + (s & 1) + (PY0 - DV0);
                    exp[m] = ae_expect(np, d->words, row, cols[s >> 1]);
                    got[m] = pr_col[i][s];
                    if (pr[i][s].data && exp[m] != got[m] && okk) {
                        okk = 0;
                        bad = m;
                    }
                    if (!pr[i][s].data)
                        exp[m] = got[m] = 0xff;
                    m++;
                }
            ok = okk;
        }
        amt_case_open(d->id, ok ? AMT_ST_PASS : AMT_ST_FAIL);
        if (!ok) {
            int row = pstep * (bad / 4) + (bad & 1) + (PY0 - DV0);
            json_obj("expect");
            json_hexn("index", exp[bad], 2);
            json_int("words_per_line", d->words);
            json_obj_end();
            json_obj("got");
            json_hexn("index", got[bad], 2);
            json_bytes("all", got, pr_n * 4);
            json_obj_end();
            json_obj("in");
            json_int("row", row);
            json_int("word_col", cols[(bad & 3) >> 1]);
            json_bytes("expect_all", exp, pr_n * 4);
            json_str("pattern", "plane p word w (from BPLxPT) = $FFFF if bit p of w else 0; odd planes "
                     "follow BPL1MOD, even planes BPL2MOD; index = (w1 & $55) | (w2 & $AA)");
            json_obj_end();
            json_str("why", d->res ? "hires: each line fetches 2*((DDFSTOP-DDFSTRT)/8+1) words per plane, "
                     "then adds the modulo (HRM 3: data fetch, modulo)"
                     : "lores: each line fetches (DDFSTOP-DDFSTRT)/8+1 words per plane, then adds the "
                     "plane's modulo (BPL1MOD odd, BPL2MOD even) to the pointer (HRM 3: data fetch, modulo)");
        }
        json_scene();
        amt_case_close();
    }
}

/* ------------------------------------------------------------ scrolling */
static void edge_scene(int res, int pl, int np)
{
    UBYTE col[20];
    int c, w = std_words(res);
    for (c = 0; c < 20; c++)
        col[c] = (UBYTE)(c >= 10 ? (1 << pl) : 0);
    reset_scene();
    sc.con0 = res_con0(res, np);
    std_ddf(res);
    sc.mod1 = sc.mod2 = (WORD)-fill_cells(np, w / 20, 20, col);
}

static void scroll_tests(void)
{
    UWORD par[16];
    int exp[16], got[16], e0 = -1, i, f, l, ok, res;
    /* PF1 and PF2 delays, lores and hires */
    for (res = 0; res < 2; res++) {
        static const char *const id1[2] = { "scroll_pf1_lores", "scroll_pf1_hires" };
        static const char *const id2[2] = { "scroll_pf2_lores", "scroll_pf2_hires" };
        int pf;
        for (pf = 0; pf < 2; pf++) {
            /* hires: delays 0-7 only (8-15 exceed one hires fetch, not specified) */
            int n = res ? (AMT_QUICK() ? 4 : 8) : (AMT_QUICK() ? 4 : 16);
            ok = 1;
            for (i = 0; i < n; i++) {
                int s = AMT_QUICK() ? i * (res ? 2 : 5) : i;
                UWORD hb = (UWORD)(origin[res] + 160 - 4);
                edge_scene(res, pf, 2);
                sc.con1 = (UWORD)(pf ? s << 4 : s);
                /* the other playfield gets the opposite delay: must not matter */
                sc.con1 |= (UWORD)(pf ? (7 - (s & 7)) : (7 - (s & 7)) << 4);
                par[i] = sc.con1;
                edge_measure(hb, 24, pf, 2, &f, &l, 0);
                got[i] = f < 0 ? -1 : hb + f;
                if (i == 0)
                    e0 = got[0] - s;
                exp[i] = e0 + s;
                if (got[i] != exp[i])
                    ok = 0;
            }
            display_off();
            if (ok)
                amt_case(pf ? id2[res] : id1[res], AMT_ST_PASS, 0);
            else
                fail_edges(pf ? id2[res] : id1[res], "BPLCON1", par, exp, got, n,
                           "a BPLCON1 delay of n moves that playfield (PF1H: odd planes, PF2H: even "
                           "planes) n lores pixels right (lores 0-15, hires 0-7, i.e. 2 hires "
                           "pixels per step); the other playfield's delay must not move it "
                           "(HRM 3: scrolling)");
        }
    }
    /* AGA fine scroll: PF1H1-0 in BPLCON1 bits 9-8 (35 ns = 1/4 lores) */
    if (!aga) {
        amt_case("scroll_aga_fine", AMT_ST_SKIP, "fine scroll bits need AGA");
        amt_case("spr_aga_shres_position", AMT_ST_SKIP, "sprite SH1/SH0 positions need AGA");
        return;
    }
    {
        static const UWORD fv[8] = { 0x0000, 0x0100, 0x0200, 0x0300, 0x0001, 0x0101, 0x0302, 0x0005 };
        static const UBYTE fs[8] = { 0, 1, 2, 3, 4, 5, 11, 20 };
        UWORD hb = (UWORD)(origin[0] + 160 - 4);
        int base = -1, lo_first = -1;
        ok = 1;
        /* lores reference with lores probes for the sprite fine-position check */
        edge_scene(0, 0, 1);
        edge_measure(hb, 12, 0, 1, &f, &l, 0);
        lo_first = f < 0 ? -1 : hb + f;
        for (i = 0; i < 8; i++) {
            edge_scene(0, 0, 1);
            sc.con1 = fv[i];
            par[i] = fv[i];
            edge_measure(hb, 4 * 12, 0, 1, &f, &l, 1);   /* shres steps */
            got[i] = f < 0 ? -1 : hb * 4 + f;
            if (i == 0)
                base = got[0];
            exp[i] = base + fs[i];
            if (got[i] != exp[i])
                ok = 0;
        }
        display_off();
        if (ok)
            amt_case("scroll_aga_fine", AMT_ST_PASS, 0);
        else
            fail_edges("scroll_aga_fine", "BPLCON1 (AGA)", par, exp, got, 8,
                       "AGA: PF1H1-0 (BPLCON1 bits 9-8) add 1/4 lores pixel steps to the PF1H5-2 "
                       "delay; positions are sprite H*4+SH (shres units) of the first hit (AGA "
                       "documentation: BPLCON1)");
        /* a lores probe at H covers shres 4H..4H+3: the shres first hit q0 must
         * satisfy 4*H0-3 <= q0 <= 4*H0 */
        ok = lo_first >= 0 && base >= 4 * lo_first - 3 && base <= 4 * lo_first;
        amt_case_open("spr_aga_shres_position", ok ? AMT_ST_PASS : AMT_ST_FAIL);
        if (!ok) {
            json_obj("expect");
            json_int("shres_first_min", 4 * lo_first - 3);
            json_int("shres_first_max", 4 * lo_first);
            json_obj_end();
            json_obj("got");
            json_int("shres_first", base);
            json_int("lores_first", lo_first);
            json_obj_end();
            json_str("why", "AGA SPRxCTL SH1/SH0 (bits 4,3) place a sprite in 1/4 lores pixel steps; "
                     "the shres-step edge scan must agree with the lores-step one (AGA documentation)");
        }
        amt_case_close();
    }
}

/* ------------------------------------------------------------ AGA fetch modes */
static void fmode_tests(void)
{
    static const char *const ids[3] = { "fmode_1_lores", "fmode_2_lores", "fmode_3_lores" };
    static const UWORD fm[3] = { 1, 2, 3 };
    static const UBYTE unit[3] = { 2, 2, 4 };
    int k;
    for (k = 0; k < 3; k++) {
        UBYTE g[8], e[8];
        int W, ok, i, edge = fm[k] == 2 ? 32 : 16;
        if (!aga) {
            amt_case(ids[k], AMT_ST_SKIP, "FMODE needs AGA");
            continue;
        }
        UWORD of;
        int ff, fl;
        reset_scene();
        sc.con0 = res_con0(0, 8);
        sc.fmode = fm[k];
        sc.diwstrt = (UWORD)((PY0 << 8) | 0x81);
        ae_setup(8);
        /* BPAGEM without BPL32 repeats the aligned 16-bit word: its first
         * nonzero AE word is word 2, not word 1. */
        pstep = 0;
        edge_measure((UWORD)(origin[0] + edge - 12), 24, fm[k] == 2 ? 1 : 0, 8, &ff, &fl, 0);
        pstep = 3;
        of = ff >= 0 ? (UWORD)(origin[0] + edge - 12 + ff - edge) : origin[0];
        reset_probes();
        pr_n = 2;
        for (i = 0; i < 2; i++) {
            static const UBYTE col[4] = { 1, 0, 6, 5 };
            int s;
            for (s = 0; s < 4; s++) {
                pr[i][s].h = (UWORD)(of + 16 * col[s] + 6);
                pr[i][s].dy = (UBYTE)(s & 1);
                pr[i][s].data = 0xf000;
            }
        }
        apply();
        scan_colors(8);
        display_off();
        for (i = 0; i < 8; i++)
            g[i] = pr_col[i / 4][i % 4];
        /* row 0: words 1 and 6 in order; row 1 starts at word W */
        W = g[1];
        e[0] = 1;
        e[1] = (UBYTE)W;
        e[2] = 6;
        e[3] = (UBYTE)(W + 5);
        e[4] = (UBYTE)(3 * W + 1);
        e[5] = (UBYTE)(4 * W);
        e[6] = (UBYTE)(3 * W + 6);
        e[7] = (UBYTE)(4 * W + 5);
        if (fm[k] == 2)
            for (i = 0; i < 8; i++)
                e[i] &= 0xfe;
        ok = ff >= 0 && W >= 20 && (W % unit[k]) == 0;
        for (i = 0; i < 8; i++)
            if (g[i] != e[i])
                ok = 0;
        amt_case_open(ids[k], ok ? AMT_ST_PASS : AMT_ST_FAIL);
        if (!ok) {
            json_obj("expect");
            json_bytes("index", e, 8);
            json_str("words_per_line", "multiple of the fetch unit and >= 20");
            json_obj_end();
            json_obj("got");
            json_bytes("index", g, 8);
            json_obj_end();
            json_obj("in");
            json_str("probes", "rows 0,1,0,1,3,4,3,4 at words 1,0,6,5,1,0,6,5 of an "
                     "address-encoded 8-plane playfield (plane p word w = bit p of w)");
            json_int("fetch_unit_words", unit[k]);
            json_obj_end();
            json_str("why", "AGA FMODE 1 (32-bit), 2 (16-bit page mode) and 3 (64-bit) fetches: "
                     "mode 2 repeats the aligned even word; modes 1/3 preserve memory order. "
                     "Every row starts W words after the previous one with modulo 0, "
                     "and W is a whole number of fetch units (AGA documentation: FMODE)");
        }
        json_obj("ctx");
        json_int("words_per_line", W);
        json_hexn("origin_fmode", of, 3);
        json_int("origin_found", ff >= 0);
        json_obj_end();
        json_scene();
        amt_case_close();
    }
}

/* ------------------------------------------------------------ AGA sprites */
static void spr_wide(int s, UWORD y, UWORD h, int words, const UWORD *data)
{
    UWORD *q = spr_list[s];
    int i;
    for (i = 0; i < 4 * words + 2 * words; i++)
        q[i] = 0;
    q[0] = (UWORD)((y << 8) | ((h >> 1) & 0xff));
    q[words] = (UWORD)(((y + 1) << 8) | (h & 1));
    for (i = 0; i < words; i++)
        q[2 * words + i] = data[i];
    /* DATB words already 0, then 2*words zero words end the list */
}

static void spr_aga_tests(void)
{
    static const char *const ids[4] = { "spr_aga_width_16", "spr_aga_width_32", "spr_aga_width_32_spagem",
                                         "spr_aga_width_64" };
    static const UWORD fm[4] = { 0x0000, 0x0004, 0x0008, 0x000c };
    static const UBYTE wds[4] = { 1, 2, 2, 4 };
    static const UBYTE exp[4] = { 0x01, 0x03, 0x03, 0x0f };
    static const UBYTE col5[5] = { 1, 2, 4, 8, 16 };
    int k;
    for (k = 0; k < 4; k++) {
        UWORD data[4];
        UBYTE got;
        int i, ok;
        if (!aga) {
            amt_case(ids[k], AMT_ST_SKIP, "sprite widths need AGA");
            continue;
        }
        for (i = 0; i < 4; i++)
            data[i] = 0xffff;
        data[0] = 0x7fff;
        data[wds[k] - 1] &= 0xfffe;
        reset_scene();
        sc.con0 = res_con0(0, 5);
        sc.fmode = fm[k];
        {
            UBYTE c20[20];
            int c;
            for (c = 0; c < 20; c++)
                c20[c] = (c >= 4 && c < 9) ? col5[c - 4] : 0;
            sc.mod1 = sc.mod2 = (WORD)-fill_cells(5, 1, 20, c20);
        }
        sc.flags |= SF_CUSTSPR;
        spr_all_empty();
        reset_probes();
        pr_n = 1;
        pr[0][0].data = 1;               /* collect slot 0 */
        spr_wide(0, PY0, (UWORD)(origin[0] + 64), wds[k], data);
        for (i = 2; i < 8; i += 2) {     /* other even sprites: empty lists in this width */
            UWORD *q = spr_list[i];
            int j;
            for (j = 0; j < 2 * wds[k]; j++)
                q[j] = 0;
        }
        {
            UWORD *q = spr_null;
            for (i = 0; i < 8; i++)
                q[i] = 0;
        }
        apply();
        scan_colors(5);
        display_off();
        got = pr_col[0][0];
        ok = got == exp[k];
        amt_case_open(ids[k], ok ? AMT_ST_PASS : AMT_ST_FAIL);
        if (!ok) {
            json_obj("expect");
            json_hexn("planes_hit", exp[k], 2);
            json_obj_end();
            json_obj("got");
            json_hexn("planes_hit", got, 2);
            json_obj_end();
            json_obj("in");
            json_hexn("fmode", fm[k], 4);
            json_int("sprite_px", 16 * wds[k]);
            json_str("layout", "sprite 0 at playfield x=64, data all ones except the outer pixels; "
                     "cells x=64,80,96,112,128 have colours 1,2,4,8,16");
            json_str("format", "POS and CTL each in the first word of one fetch unit, then DATA, DATB "
                     "one fetch unit each");
            json_obj_end();
            json_str("why", "AGA FMODE SPR32/SPAGEM select 32-pixel and both 64-pixel sprites; "
                     "a sprite covers 16/32/64 lores pixels (AGA documentation: FMODE, sprites)");
        }
        amt_case_close();
    }
    /* sprite resolution BPLCON3 SPRES */
    {
        static const char *const rid[4] = { "spr_aga_spres_0_default", "spr_aga_spres_1_lores",
                                            "spr_aga_spres_2_hires", "spr_aga_spres_3_shres" };
        static const UBYTE ea[4] = { 3, 3, 1, 1 }, eb[4] = { 3, 3, 3, 1 };
        static const UBYTE col2[2] = { 1, 2 };
        int r;
        for (r = 0; r < 4; r++) {
            UBYTE a, b;
            if (!aga) {
                amt_case(rid[r], AMT_ST_SKIP, "BPLCON3 SPRES checked on AGA only");
                continue;
            }
            reset_scene();
            sc.con0 = res_con0(0, 2);
            sc.con3 = (UWORD)(0x0c00 | (r << 6));
            lores_row(2, 2, col2);
            reset_probes();
            pr_n = 1;
            pr[0][0].h = (UWORD)(origin[0] + 64 + 4);
            pr[0][0].data = 0x7ffe;
            pr[0][2].h = (UWORD)(origin[0] + 64 + 10);
            pr[0][2].data = 0x7ffe;
            apply();
            scan_colors(2);
            display_off();
            a = pr_col[0][0];
            b = pr_col[0][2];
            amt_case_open(rid[r], (a == ea[r] && b == eb[r]) ? AMT_ST_PASS : AMT_ST_FAIL);
            if (!(a == ea[r] && b == eb[r])) {
                json_obj("expect");
                json_hexn("a", ea[r], 2);
                json_hexn("b", eb[r], 2);
                json_obj_end();
                json_obj("got");
                json_hexn("a", a, 2);
                json_hexn("b", b, 2);
                json_obj_end();
                json_obj("in");
                json_hexn("bplcon3", sc.con3, 4);
                json_str("layout", "lores playfield, cells of 16 px alternating colour 1/2; sprite a at "
                         "cell start+4, b at +10, data $7FFE");
                json_obj_end();
                json_str("why", "BPLCON3 SPRES 00/01 = lores sprite pixels (16 px reach the next cell), "
                         "10 = hires (8 px: only b reaches it), 11 = shres (4 px: neither) "
                         "(AGA documentation: BPLCON3)");
            }
            amt_case_close();
        }
    }
}

/* ------------------------------------------------------------ AGA palette */
static UWORD col_read(int n) { return CU(COLOR00 + 2 * n); }

static void pal_tests(void)
{
    int b, i, bad = -1, okc = 1;
    ULONG want = 0, have = 0;
    if (!aga) {
        amt_case("pal_aga_rdram_24bit", AMT_ST_SKIP, "colour registers are write-only before AGA "
                 "(AGA BPLCON2 RDRAM)");
        amt_case("pal_aga_loct_copy", AMT_ST_SKIP, "AGA only");
        return;
    }
    display_off();
    amt_set_sr(0x2700);
    CU(BPLCON2) = 0;
    for (b = 0; b < 8; b++) {
        CU(BPLCON3) = (UWORD)(0x0c00 | (b << 13));
        for (i = 0; i < 32; i++) {
            ULONG c = pal256[b * 32 + i];
            CU(COLOR00 + 2 * i) = (UWORD)(((c >> 12) & 0xf00) | ((c >> 8) & 0xf0) | ((c >> 4) & 0xf));
        }
        CU(BPLCON3) = (UWORD)(0x0e00 | (b << 13));
        for (i = 0; i < 32; i++) {
            ULONG c = pal256[b * 32 + i];
            CU(COLOR00 + 2 * i) = (UWORD)(((c >> 8) & 0xf00) | ((c >> 4) & 0xf0) | (c & 0xf));
        }
    }
    CU(BPLCON2) = 0x0100;                 /* RDRAM */
    for (b = 0; b < 8 && bad < 0; b++)
        for (i = 0; i < 32; i++) {
            ULONG c = pal256[b * 32 + i], hi, lo, got;
            CU(BPLCON3) = (UWORD)(0x0c00 | (b << 13));
            hi = col_read(i) & 0x0fff;
            CU(BPLCON3) = (UWORD)(0x0e00 | (b << 13));
            lo = col_read(i) & 0x0fff;
            got = ((hi & 0xf00) << 12) | ((lo & 0xf00) << 8) | ((hi & 0xf0) << 8) | ((lo & 0xf0) << 4) |
                  ((hi & 0xf) << 4) | (lo & 0xf);
            if (got != c) {
                bad = b * 32 + i;
                want = c;
                have = got;
                break;
            }
        }
    /* LOCT=0 write copies the nibble into the low half */
    CU(BPLCON2) = 0;
    CU(BPLCON3) = 0x0c00 | (3 << 13);
    CU(COLOR00 + 2 * 7) = 0x05a3;
    CU(BPLCON2) = 0x0100;
    {
        UWORD lo;
        CU(BPLCON3) = 0x0e00 | (3 << 13);
        lo = col_read(7) & 0x0fff;
        okc = lo == 0x05a3;
        CU(BPLCON3) = 0x0c00;
        CU(BPLCON2) = 0;
        amt_set_sr(0x2000);
        amt_case_open("pal_aga_loct_copy", okc ? AMT_ST_PASS : AMT_ST_FAIL);
        if (!okc) {
            json_obj("expect");
            json_hexn("low_nibbles", 0x05a3, 4);
            json_obj_end();
            json_obj("got");
            json_hexn("low_nibbles", lo, 4);
            json_obj_end();
            json_obj("in");
            json_str("seq", "bank 3, LOCT=0: COLOR07=$05A3; read back with RDRAM, LOCT=1");
            json_obj_end();
            json_str("why", "AGA: a colour write with LOCT=0 sets the high nibbles and copies them to "
                     "the low nibbles (12-bit compatibility), so the low nibbles read $5A3 "
                     "(AGA documentation: BPLCON3 LOCT, BPLCON2 RDRAM)");
        }
        amt_case_close();
    }
    amt_case_open("pal_aga_rdram_24bit", bad < 0 ? AMT_ST_PASS : AMT_ST_FAIL);
    if (bad >= 0) {
        json_obj("expect");
        json_hexn("rgb", want, 6);
        json_obj_end();
        json_obj("got");
        json_hexn("rgb", have, 6);
        json_obj_end();
        json_obj("in");
        json_int("colour", bad);
        json_str("seq", "per bank (BPLCON3 bits 15-13): 32 writes with LOCT=0 (high nibbles), 32 with "
                 "LOCT=1 (low nibbles); then BPLCON2 RDRAM=1 and read each register with LOCT=0/1");
        json_obj_end();
        json_str("why", "AGA: the 256 x 24-bit palette is written through 8 banks of 32 registers, "
                 "high/low nibbles selected by LOCT, and read back with BPLCON2 RDRAM "
                 "(AGA documentation: BPLCON2/BPLCON3)");
    }
    amt_case_close();
}

/* ------------------------------------------------------------ IDs */
/* DENISEID/LISAID checked against an independent source: the Agnus ID in
 * VPOSR bits 14-8 (read here, not taken from the probe). Rules from the
 * HRM appendix C and the AGA documentation:
 *   Alice (Agnus ID $22/$23, NTSC bit 4 ignored) => Lisa: bits 7-0 = $F8;
 *   no Alice => no Lisa: bits 7-0 != $F8;
 *   an ID that reads $FC (ECS Denise) or $F8 must read the same every time.
 * Bits 15-8 are not specified; an OCS Denise has no register (floating bus):
 * nothing else is asserted. */
static void id_tests(void)
{
    UWORD v[16];
    int i, stable = 1, ok;
    UBYTE agnus = (UBYTE)((CU(VPOSR) >> 8) & 0x7f), k = (UBYTE)(agnus & 0x6f), lo;
    int alice = k == 0x22 || k == 0x23;
    const char *ac = alice ? "alice" : (k == 0x20 || k == 0x21) ? "ecs_8372" : k == 0 ? "ocs" : "other";
    const char *dc, *why;
    for (i = 0; i < 16; i++)
        v[i] = CU(DENISEID);
    for (i = 1; i < 16; i++)
        if ((v[i] & 0xff) != (v[0] & 0xff))
            stable = 0;
    lo = (UBYTE)v[0];
    dc = !stable ? "unstable" : lo == 0xf8 ? "lisa" : lo == 0xfc ? "ecs_denise" : "no_id";
    if (alice) {
        ok = stable && lo == 0xf8;
        why = "Agnus is Alice (VPOSR ID $22/$23), so Denise must be Lisa: LISAID bits 7-0 = $F8 on "
              "every read (AGA documentation: LISAID)";
    } else if (!ecsd) {
        amt_case("deniseid", AMT_ST_UNTESTABLE,
                 "no independently corroborated Denise ID register; OCS floating-bus coincidences cannot identify Denise");
        return;
    } else {
        ok = lo != 0xf8 && (stable || (lo != 0xfc && lo != 0xf8));
        why = "without Alice there is no Lisa, so bits 7-0 must not read $F8; an ECS Denise ID ($FC, "
              "HRM appendix C) must read the same every time";
    }
    amt_case_open("deniseid", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        if (alice)
            json_hexn("low_byte", 0xf8, 2);
        else
            json_str("low_byte", "not $F8");
        json_int("stable", 1);
        json_obj_end();
        json_obj("got");
        json_hexn("low_byte", lo, 2);
        json_int("stable", stable);
        json_obj_end();
        json_str("why", why);
    }
    json_obj("ctx");
    json_str("agnus_class", ac);
    json_str("undef_denise_class", dc);
    json_obj_end();
    /* raw values: bits 15-8 unspecified, OCS reads the floating bus */
    json_obj("timing");
    json_hexn("raw", v[0], 4);
    json_hexn("agnus_id", agnus, 2);
    json_obj_end();
    amt_case_close();
}

/* ------------------------------------------------------------ DMA slots (timing) */
static ULONG count_reads(void)
{
    volatile UWORD *p = (volatile UWORD *)(pool + OFF_WORK);
    ULONG n = 0, budget = 1000000;
    UWORD sr = amt_get_sr();
    wait_vbl();
    amt_set_sr(0x2700);
    wait_line(0x60);
    while (hw_vline() < 0xa0) {
        if (!--budget) {
            amt_set_sr(sr);
            amt_abort(AMT_ABORT_WATCHDOG);
        }
        (void)p[0]; (void)p[0]; (void)p[0]; (void)p[0];
        (void)p[0]; (void)p[0]; (void)p[0]; (void)p[0];
        n += 8;
    }
    amt_set_sr(sr);
    return n;
}

static void dma_case(const char *id, int res, int np, UWORD fm)
{
    ULONG n;
    reset_scene();
    sc.flags |= SF_CUSTSPR;
    spr_all_empty();
    if (np >= 0) {
        sc.con0 = res_con0(res, np);
        std_ddf(res);
        sc.fmode = fm;
        apply();
    } else {
        display_off();
        wait_frames(1);
    }
    n = count_reads();
    display_off();
    amt_case_open(id, AMT_ST_PASS);
    json_obj("timing");
    json_str("unit", "CPU word reads of chip RAM (8 per VHPOSR poll) completed in lines $60-$9F, "
             "standard DDF/DIW; fewer free slots with more planes (HRM 6: DMA time slots)");
    json_int("reads", (long)n);
    json_int("lines", 64);
    json_obj_end();
    amt_case_close();
}

static void dma_tests(void)
{
    static const char *const lo[7] = { "dma_cpu_lores_0bpl", "dma_cpu_lores_1bpl", "dma_cpu_lores_2bpl",
                                       "dma_cpu_lores_3bpl", "dma_cpu_lores_4bpl", "dma_cpu_lores_5bpl",
                                       "dma_cpu_lores_6bpl" };
    static const char *const hi[4] = { "dma_cpu_hires_1bpl", "dma_cpu_hires_2bpl", "dma_cpu_hires_3bpl",
                                       "dma_cpu_hires_4bpl" };
    int n;
    dma_case("dma_cpu_off", 0, -1, 0);
    for (n = 0; n <= 6; n++)
        dma_case(lo[n], 0, n, 0);
    for (n = 1; n <= 4; n++)
        dma_case(hi[n - 1], 1, n, 0);
    if (ecsd && ecsa)
        dma_case("dma_cpu_shres_2bpl", 2, 2, 0);
    else
        amt_case("dma_cpu_shres_2bpl", AMT_ST_SKIP, "superhires needs ECS");
    if (aga) {
        dma_case("dma_cpu_aga_lores_8bpl_f0", 0, 8, 0);
        dma_case("dma_cpu_aga_lores_8bpl_f3", 0, 8, 3);
    } else {
        amt_case("dma_cpu_aga_lores_8bpl_f0", AMT_ST_SKIP, "AGA only");
        amt_case("dma_cpu_aga_lores_8bpl_f3", AMT_ST_SKIP, "AGA only");
    }
}

/* ------------------------------------------------------------ screenshots only */
static void shot_only(const char *id, const char *what)
{
    int r = shot(id);
    display_off();
    amt_case_open(id, r > 0 ? AMT_ST_PASS : AMT_ST_UNTESTABLE);
    if (r <= 0)
        json_str("why", "host did not act on request screenshot");
    json_str("pattern", what);
    json_scene();
    json_shot(r);
    amt_case_close();
}

static void shot_tests(void)
{
    static UWORD ex[64];
    int i, n;
    UBYTE c20[20];
    /* dual playfield priorities: 4 bands with different BPLCON2 */
    reset_scene();
    sc.con0 = res_con0(0, 6) | 0x0400;
    sc.flags |= SF_PAL32 | SF_CUSTSPR | SF_ODDSPR;
    for (i = 0; i < 20; i++)
        c20[i] = (UBYTE)((i * 0x0b + 0x13) & 0x3f);
    sc.mod1 = sc.mod2 = (WORD)-fill_cells(6, 1, 20, c20);
    for (i = 0; i < 8; i++) {
        spr_begin(i);
        spr_ent(0x48, 0xe8, (UWORD)(0x90 + 24 * i), (UWORD)((i & 1) ? 0x80 : 0), 0x0ff0, 0xf00f);
        spr_end();
    }
    n = 0;
    for (i = 1; i < 4; i++) {
        ex[n++] = (UWORD)(((DV0 + 40 * i) << 8) | 0x07);
        ex[n++] = 0xfffe;
        ex[n++] = BPLCON2;
        ex[n++] = (UWORD)((i & 1 ? 0x0040 : 0) | (i * 9));
    }
    sc.extra = ex;
    sc.nextra = n;
    apply();
    shot_only("shot_dpf_priorities", "dual playfield 3+3 planes, 8 sprites (odd ones attached), "
              "BPLCON2 = $0000/$0049/$0012/$005B in 40-line bands");
    /* ECS border: BRDRBLNK, BRDSPRT */
    if (ecsd) {
        reset_scene();
        sc.con0 = res_con0(0, 2) | 0x0001;   /* ECSENA */
        sc.con3 = 0x0c22;                   /* BRDRBLNK + BRDSPRT */
        sc.flags |= SF_PAL32 | SF_CUSTSPR;
        sc.diwstrt = (UWORD)((0x60 << 8) | 0xa1);
        sc.diwstop = (UWORD)((0xc0 << 8) | 0xa1);
        lores_row(2, 4, (const UBYTE *)"\0\1\2\3");
        spr_all_empty();
        spr_begin(0);
        spr_ent(0x50, 0xd0, 0x90, 0, 0xffff, 0x00ff);
        spr_end();
        apply();
        shot_only("shot_ecs_border", "ECSENA, BPLCON3 BRDRBLNK+BRDSPRT: black border, sprite 0 "
                  "visible across the left border");
    } else
        amt_case("shot_ecs_border", AMT_ST_SKIP, "BRDRBLNK/BRDSPRT need ECS Denise");
    /* AGA BPLCON4 bitplane XOR and sprite colour banks */
    if (aga) {
        reset_scene();
        sc.con0 = res_con0(0, 8);
        sc.con4 = 0x5a37;                   /* BPLAM $5A, ESPRM 3, OSPRM 7 */
        sc.flags |= SF_PAL256 | SF_CUSTSPR | SF_ODDSPR;
        for (i = 0; i < 20; i++)
            c20[i] = (UBYTE)(i * 13);
        sc.mod1 = sc.mod2 = (WORD)-fill_cells(8, 1, 20, c20);
        for (i = 0; i < 8; i++) {
            spr_begin(i);
            spr_ent(0x50, 0xe0, (UWORD)(0x90 + 24 * i), 0, 0x3ffc, 0x0ff0);
            spr_end();
        }
        apply();
        shot_only("shot_aga_bplcon4", "8 planes, 24-bit palette, BPLCON4 = $5A37 (BPLAM XOR $5A, "
                  "sprite banks 3/7), 8 sprites");
    } else
        amt_case("shot_aga_bplcon4", AMT_ST_SKIP, "BPLCON4 needs AGA");
}

/* ------------------------------------------------------------ main */
static void body(void)
{
    int r;
    default_palette();
    for (r = 0; r < 3; r++) {
        if (r == 2 && !(ecsd && ecsa)) {
            amt_case("align_shres", AMT_ST_SKIP, "superhires needs ECS Agnus and Denise");
            continue;
        }
        calib_origin(r);
    }
    mode_tests();
    clx_spf_odd();
    clx_pfpf_tests();
    clx_sprpf_case("clx_spr_pf_dpf", 1, AMT_QUICK() ? 12 : 48);
    clx_sprpf_case("clx_spr_pf_spf", 0, AMT_QUICK() ? 12 : 48);
    clx_sprspr_tests();
    clx_clear_test();
    clx_clxcon2_reset();
    clx_attached();
    spr_manual();
    spr_vertical();
    diw_h_tests();
    diwhigh_test();
    diw_v_test();
    ddf_tests();
    scroll_tests();
    fmode_tests();
    spr_aga_tests();
    pal_tests();
    id_tests();
    dma_tests();
    shot_tests();
}

void amt_run_display(void)
{
    int code, i;
    pool = (UBYTE *)amt_chip_pool();
    aga = amt_probe_result.chipset && amt_probe_result.chipset[0] == 'A';
    ecsd = amt_probe_result.ecs_denise || aga;
    ecsa = amt_probe_result.ecs_agnus || aga;
    pal = amt_probe_result.pal;
    for (i = 0; i < 8; i++) {
        plane[i] = (UWORD *)(pool + i * PL_BYTES);
        spr_list[i] = (UWORD *)(pool + OFF_SPR + i * SPR_BYTES);
    }
    spr_null = (UWORD *)(pool + OFF_NULL);
    spr_null[0] = spr_null[1] = spr_null[2] = spr_null[3] = 0;
    for (i = 0; i < 8; i++)
        spr_list[i][0] = spr_list[i][1] = 0;
    code = amt_try(body);
    amt_set_sr(0x2700);
    CU(DMACON) = DMAF_BPLEN | DMAF_SPREN | DMAF_COPEN;
    CU(CLXCON) = 0;
    if (aga) {
        CU(FMODE) = 0;
        CU(BPLCON4) = 0x0011;
    }
    if (ecsd)
        CU(BPLCON3) = 0x0c00;
    CU(BPLCON1) = 0;
    CU(BPLCON2) = 0x0024;
    CU(BPLCON0) = 0x0200;
    for (i = 0; i < 8; i++)
        CU(SPR0CTL + 8 * i) = 0;
    hw_restore_copper();
    amt_set_sr(0x2000);
    if (code)
        amt_abort(code);
}
