/* rtg: Cirrus Logic GD542x/543x/5446 suite (Picasso II/II+/IV, Piccolo,
 * SD64, Spectrum, Graffity, Pixel64).
 * Reference: Cirrus Logic GD542X Technical Reference Manual, GD543X/5446
 * TRM; IBM VGA technical reference for the standard registers. */
#include "rtg.h"

static struct vga_state cst;
static UBYTE p4_bank, gr31_pre, gr33_pre;

/* Cirrus 8-bit ROP codes (GR32), as in the TRM / XFree86 cirRop[] */
static const UBYTE cl_rops[16] = {
    0x00, 0x05, 0x06, 0x09, 0x0b, 0x0d, 0x0e, 0x50,
    0x59, 0x6d, 0x90, 0x95, 0xad, 0xd0, 0xd6, 0xda
};

static UBYTE rop8(UBYTE rop, UBYTE s, UBYTE d)
{
    switch (rop) {
    case 0x00: return 0;
    case 0x05: return s & d;
    case 0x06: return d;
    case 0x09: return s & ~d;
    case 0x0b: return ~d;
    case 0x0d: return s;
    case 0x0e: return 0xff;
    case 0x50: return ~s & d;
    case 0x59: return s ^ d;
    case 0x6d: return s | d;
    case 0x90: return ~(s | d);
    case 0x95: return ~(s ^ d);
    case 0xad: return s | ~d;
    case 0xd0: return ~s;
    case 0xd6: return ~s | d;
    default:   return ~(s & d);   /* 0xda */
    }
}

static int cl_wait(void)
{
    ULONG f0 = amt_frames_now();
    while (GR_R(0x31) & 0x09) {
        if (amt_frames_now() - f0 > 25)
            return 0;
    }
    return 1;
}

static int cl_blt(ULONG dst, ULONG src, UWORD w, UWORD h, UWORD dp, UWORD sp, UBYTE mode, UBYTE rop)
{
    GR_W(0x20, (UBYTE)(w - 1)); GR_W(0x21, (UBYTE)((w - 1) >> 8));
    GR_W(0x22, (UBYTE)(h - 1)); GR_W(0x23, (UBYTE)((h - 1) >> 8));
    GR_W(0x24, (UBYTE)dp); GR_W(0x25, (UBYTE)(dp >> 8));
    GR_W(0x26, (UBYTE)sp); GR_W(0x27, (UBYTE)(sp >> 8));
    GR_W(0x28, (UBYTE)dst); GR_W(0x29, (UBYTE)(dst >> 8)); GR_W(0x2a, (UBYTE)(dst >> 16));
    GR_W(0x2c, (UBYTE)src); GR_W(0x2d, (UBYTE)(src >> 8)); GR_W(0x2e, (UBYTE)(src >> 16));
    GR_W(0x30, mode);
    GR_W(0x32, rop);
    GR_W(0x31, 0x02);
    return cl_wait();
}

/* 5436/5446 autostart (GR31 bit 7): the BLT starts when GR2A (destination
 * address high byte) is written, so everything else is programmed first */
static int cl_blt_auto(ULONG dst, ULONG src, UWORD w, UWORD h, UWORD dp, UWORD sp, UBYTE mode, UBYTE rop)
{
    GR_W(0x20, (UBYTE)(w - 1)); GR_W(0x21, (UBYTE)((w - 1) >> 8));
    GR_W(0x22, (UBYTE)(h - 1)); GR_W(0x23, (UBYTE)((h - 1) >> 8));
    GR_W(0x24, (UBYTE)dp); GR_W(0x25, (UBYTE)(dp >> 8));
    GR_W(0x26, (UBYTE)sp); GR_W(0x27, (UBYTE)(sp >> 8));
    GR_W(0x2c, (UBYTE)src); GR_W(0x2d, (UBYTE)(src >> 8)); GR_W(0x2e, (UBYTE)(src >> 16));
    GR_W(0x30, mode);
    GR_W(0x32, rop);
    GR_W(0x31, 0x80);
    GR_W(0x28, (UBYTE)dst); GR_W(0x29, (UBYTE)(dst >> 8));
    GR_W(0x2a, (UBYTE)(dst >> 16));
    {
        int ok = cl_wait();
        GR_W(0x31, 0x00);
        return ok;
    }
}

#define PITCH 640
#define BX(x, y) ((ULONG)(y) * PITCH + (x))

/* fill a rectangle of the work area with seeded random bytes */
static void fill_rand(ULONG a, int w, int h)
{
    volatile UBYTE *p = rtg_cur->win[0].p;
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            p[a + (ULONG)y * PITCH + x] = (UBYTE)(amt_rand() >> 13);
}

/* read the region around a destination rect (one pixel / line border) */
static void grab(UBYTE *buf, ULONG d, int w, int h)
{
    volatile UBYTE *p = rtg_cur->win[0].p;
    int x, y, i = 0;
    for (y = -1; y <= h; y++)
        for (x = -1; x <= w; x++)
            buf[i++] = p[(LONG)d + (LONG)y * PITCH + x];
}

/* compare, report; exp/got are (w+2)*(h+2) */
static void blt_report(const char *suffix, const char *why, UBYTE *exp, UBYTE *got,
                       int w, int h, int done, UBYTE mode, UBYTE rop, ULONG src, ULONG dst)
{
    int n = (w + 2) * (h + 2), i, bad = -1;
    ULONG rows = 0, nbad = 0;
    for (i = 0; i < n; i++)
        if (exp[i] != got[i]) {
            if (bad < 0) bad = i;
            nbad++;
            if (i / (w + 2) < 32) rows |= 1UL << (i / (w + 2));
        }
    if (done && bad < 0) { rtg_pass(suffix); return; }
    amt_case_open(rtg_cid(suffix), AMT_ST_FAIL);
    json_obj("expect"); json_hex("crc", crc32_update(0, exp, n));
    if (bad >= 0) json_hexn("byte", exp[bad], 2);
    json_int("done", 1); json_obj_end();
    json_obj("got"); json_hex("crc", crc32_update(0, got, n));
    if (bad >= 0) json_hexn("byte", got[bad], 2);
    json_int("done", done); json_obj_end();
    json_str("why", done ? why : "BitBLT did not finish: GR31 bits 0/3 (busy) still set after 25 frames");
    json_obj("in");
    json_hexn("mode", mode, 2); json_hexn("rop", rop, 2);
    json_hex("src", src); json_hex("dst", dst);
    json_int("w", w); json_int("h", h); json_int("pitch", PITCH);
    if (bad >= 0) { json_int("x", bad % (w + 2) - 1); json_int("y", bad / (w + 2) - 1); }
    json_obj_end();
    json_obj("diff"); json_int("bytes", (long)nbad); json_hex("rows", rows); json_str("rows_is", "bit r = region row r (row 0 = border above)"); json_obj_end();
    json_obj("ctx"); json_str("region", "dest rect plus a 1-pixel border, which must stay unchanged");
    json_hexn("gr31_found", gr31_pre, 2); json_hexn("gr33_found", gr33_pre, 2); rtg_ctx_card(); json_obj_end();
    amt_case_close();
}

static void blt_rops(void)
{
    UBYTE *before = rtg_buf, *exp = rtg_buf + 4096, *got = rtg_buf + 8192;
    UBYTE srcb[40 * 12];
    const ULONG src = BX(8, 4), dst = BX(320, 40);
    const int w = 40, h = 12;
    int r, x, y, done;
    char suf[24];
    volatile UBYTE *p = rtg_cur->win[0].p;
    for (r = 0; r < 16; r++) {
        UBYTE rop = cl_rops[r];
        const char *hx = "0123456789abcdef";
        char *d = suf;
        const char *s = "cl_blt_rop_";
        while (*s) *d++ = *s++;
        *d++ = hx[rop >> 4]; *d++ = hx[rop & 15]; *d = 0;
        fill_rand(src, w, h);
        fill_rand(dst - PITCH - 1, w + 2, h + 2);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                srcb[y * w + x] = p[src + (ULONG)y * PITCH + x];
        grab(before, dst, w, h);
        for (x = 0; x < (w + 2) * (h + 2); x++) exp[x] = before[x];
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int i = (y + 1) * (w + 2) + x + 1;
                exp[i] = rop8(rop, srcb[y * w + x], before[i]);
            }
        done = cl_blt(dst, src, w, h, PITCH, PITCH, 0x00, rop);
        grab(got, dst, w, h);
        blt_report(suf, "screen-to-screen BitBLT result differs from the same raster operation done by the CPU (GR32 ROP code per the GD542x TRM)",
                   exp, got, w, h, done, 0x00, rop, src, dst);
    }
}

static void blt_autostart(void)
{
    UBYTE *before = rtg_buf, *exp = rtg_buf + 4096, *got = rtg_buf + 8192;
    UBYTE srcb[32 * 8];
    const ULONG src = BX(16, 20), dst = BX(480, 20);
    const int w = 32, h = 8;
    int x, y, done;
    volatile UBYTE *p = rtg_cur->win[0].p;
    fill_rand(src, w, h);
    fill_rand(dst - PITCH - 1, w + 2, h + 2);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            srcb[y * w + x] = p[src + (ULONG)y * PITCH + x];
    grab(before, dst, w, h);
    for (x = 0; x < (w + 2) * (h + 2); x++) exp[x] = before[x];
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            exp[(y + 1) * (w + 2) + x + 1] = rop8(0x6d, srcb[y * w + x], before[(y + 1) * (w + 2) + x + 1]);
    done = cl_blt_auto(dst, src, w, h, PITCH, PITCH, 0x00, 0x6d);
    grab(got, dst, w, h);
    blt_report("cl_blt_autostart", "with GR31 bit 7 (autostart, GD5436/5446 TRM) set, writing GR2A must start the BLT programmed by the other registers; result differs from the CPU result",
               exp, got, w, h, done, 0x00, 0x6d, src, dst);
}

static void blt_pattern(void)
{
    UBYTE *before = rtg_buf, *exp = rtg_buf + 4096, *got = rtg_buf + 8192;
    UBYTE pat[64];
    const ULONG pa = 0xfa00, dst = BX(328, 60);
    const int w = 32, h = 16;
    int x, y, done;
    volatile UBYTE *p = rtg_cur->win[0].p;
    /* 8x8 colour pattern, 64-byte aligned, start row 0, dest x on a multiple of 8 */
    for (x = 0; x < 64; x++) { pat[x] = (UBYTE)(amt_rand() >> 9); p[pa + x] = pat[x]; }
    fill_rand(dst - PITCH - 1, w + 2, h + 2);
    grab(before, dst, w, h);
    for (x = 0; x < (w + 2) * (h + 2); x++) exp[x] = before[x];
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            exp[(y + 1) * (w + 2) + x + 1] = rop8(0x59, pat[(y & 7) * 8 + (x & 7)], before[(y + 1) * (w + 2) + x + 1]);
    done = cl_blt(dst, pa, w, h, PITCH, 8, 0x40, 0x59);
    grab(got, dst, w, h);
    blt_report("cl_blt_pattern", "8x8 pattern-copy BitBLT (GR30 bit 6) with ROP S^D differs from the CPU result (pattern row = BLT line & 7, column = x & 7)",
               exp, got, w, h, done, 0x40, 0x59, pa, dst);
}

static void blt_cexp(int transp, int pattern)
{
    UBYTE *before = rtg_buf, *exp = rtg_buf + 4096, *got = rtg_buf + 8192;
    UBYTE mono[96];
    const ULONG ma = 0xfa40, dst = BX(400 - 8 * transp - 16 * pattern, 70);
    const int w = 64, h = 12;
    const UBYTE fg = 0x0c, bg = 0x03;
    UBYTE mode = (UBYTE)(0x80 | (transp ? 0x08 : 0) | (pattern ? 0x40 : 0));
    int x, y, done, nb = pattern ? 8 : 96;
    volatile UBYTE *p = rtg_cur->win[0].p;
    for (x = 0; x < nb; x++) { mono[x] = (UBYTE)(amt_rand() >> 11); p[ma + x] = mono[x]; }
    fill_rand(dst - PITCH - 1, w + 2, h + 2);
    grab(before, dst, w, h);
    for (x = 0; x < (w + 2) * (h + 2); x++) exp[x] = before[x];
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            UBYTE m = pattern ? mono[y & 7] : mono[y * 8 + (x >> 3)];
            int bit = (m >> (7 - (x & 7))) & 1;
            int i = (y + 1) * (w + 2) + x + 1;
            if (bit) exp[i] = fg;
            else if (!transp) exp[i] = bg;
        }
    GR_W(0x00, bg);
    GR_W(0x01, fg);
    GR_W(0x10, 0); GR_W(0x11, 0);
    done = cl_blt(dst, ma, w, h, PITCH, 8, mode, 0x0d);
    GR_W(0x00, 0); GR_W(0x01, 0);
    grab(got, dst, w, h);
    blt_report(pattern ? "cl_blt_pat_cexp" : (transp ? "cl_blt_cexp_transp" : "cl_blt_cexp"),
               pattern ? "8x8 monochrome pattern colour-expansion BitBLT (GR30 bits 7,6; fg GR1, bg GR0) differs from the CPU result"
                       : transp ? "transparent colour-expansion BitBLT (GR30 bits 7,3): 1 bits = fg (GR1), 0 bits leave the destination; differs from the CPU result"
                                : "colour-expansion BitBLT from display memory (GR30 bit 7): 1 bits = fg (GR1), 0 bits = bg (GR0), MSB = leftmost pixel; differs from the CPU result",
               exp, got, w, h, done, mode, 0x0d, ma, dst);
}

static void blt_backward(void)
{
    UBYTE *before = rtg_buf, *exp = rtg_buf + 4096, *got = rtg_buf + 8192;
    UBYTE srcb[48 * 10];
    const ULONG src = BX(8, 80), dst = BX(10, 81);
    const int w = 48, h = 10;
    int x, y, done;
    volatile UBYTE *p = rtg_cur->win[0].p;
    /* overlapping move down/right: the decrementing BLT (GR30 bit 0, start
     * addresses = last byte) must give a correct (memmove) copy */
    fill_rand(src - PITCH - 1, w + 4, h + 3);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            srcb[y * w + x] = p[src + (ULONG)y * PITCH + x];
    grab(before, dst, w, h);
    for (x = 0; x < (w + 2) * (h + 2); x++) exp[x] = before[x];
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            exp[(y + 1) * (w + 2) + x + 1] = srcb[y * w + x];
    done = cl_blt(dst + (ULONG)(h - 1) * PITCH + w - 1, src + (ULONG)(h - 1) * PITCH + w - 1, w, h, PITCH, PITCH, 0x01, 0x0d);
    grab(got, dst, w, h);
    blt_report("cl_blt_backward", "overlapping BitBLT in decrement mode (GR30 bit 0, addresses of the last byte) is not a correct copy of the original source",
               exp, got, w, h, done, 0x01, 0x0d, src, dst);
}

static void cl_board_irq(int on) { rtg_board_irq(on); }
static void cl_noop_irq(int on) { (void)on; }

void rtg_cirrus(void)
{
    struct rtg_card *c = rtg_cur;
    UBYTE sr6, id, m0, r1, r2, lin;
    int i, idok = 0;
    volatile UBYTE *vram = c->win[0].p;

    if (c->kind == K_PICASSO4_Z2) {
        /* board bank register: bit 1 unmaps the flash, the VGA I/O appears
         * at +0x10000 (Amiberry/WinUAE Picasso IV model) */
        p4_bank = c->regs[0];
        c->regs[0] = (UBYTE)(p4_bank | 0x02);
    }
    m0 = vi(0x3cc);
    vga_color_io();
    sr6 = SR_R(6);
    /* SR6 key: writing 0x12 unlocks the extensions and reads back 0x12;
     * any other value locks them and reads back 0x0F (GD542x TRM) */
    SR_W(6, 0x00);
    r1 = SR_R(6);
    SR_W(6, 0x12);
    r2 = SR_R(6);
    if (r1 == 0x0f && r2 == 0x12)
        rtg_pass("cl_unlock");
    else {
        amt_case_open(rtg_cid("cl_unlock"), AMT_ST_FAIL);
        json_obj("expect"); json_hexn("locked", 0x0f, 2); json_hexn("unlocked", 0x12, 2); json_obj_end();
        json_obj("got"); json_hexn("locked", r1, 2); json_hexn("unlocked", r2, 2); json_obj_end();
        json_str("why", "SR6 (unlock extensions) must read 0x12 after writing 0x12 and 0x0F after writing any other value (GD542x TRM)");
        json_obj("ctx"); json_hexn("sr6_found", sr6, 2); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
    vga_save(&cst, 0x20, 0x40, 0x40);
    cst.misc = m0;
    /* GR31 bit 7 (autostart, 5436/5446) as found: the Picasso IV's own
     * boot ROM leaves it set, and then every GR2A write starts a BLT. It
     * is cleared before any BitBLT register is written (GR31 bit 1 starts
     * each BLT) and restored at the end. */
    gr31_pre = GR_R(0x31);
    gr33_pre = GR_R(0x33);
    GR_W(0x31, 0x00);

    /* chip id: CR27 bits 7:2 (bits 1:0 = revision) */
    id = CR_R(0x27);
    for (i = 0; i < c->nids; i++)
        if ((id & 0xfc) == (c->ids[i] & 0xfc)) idok = 1;
    if (idok) {
        amt_case_open(rtg_cid("cl_chip_id"), AMT_ST_PASS);
        json_obj("ctx"); json_hexn("cr27", id, 2); json_obj_end();
        amt_case_close();
    } else {
        amt_case_open(rtg_cid("cl_chip_id"), AMT_ST_FAIL);
        json_obj("expect"); json_hexn("cr27", c->ids[0], 2);
        if (c->ids[1] != c->ids[0]) json_hexn("cr27_alt", c->ids[1], 2);
        json_obj_end();
        json_obj("got"); json_hexn("cr27", id & 0xfc, 2); json_obj_end();
        json_str("why", "CR27 (ID) bits 7:2 do not identify the Cirrus chip this board carries (5426 0x90, 5428 0x98, 5434 0xA8, 5446 0xB8; TRM)");
        json_obj("ctx"); json_hexn("cr27_raw", id, 2); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
    /* Picasso II: 5428 is the II+, which has the vertical interrupt on INT2 */
    if (c->kind == K_PICASSO2 && (id & 0xfc) == 0x98) {
        c->irq = 2;
        c->irq_inferred = 1;
    }

    /* linear frame buffer: SR7 bits 7:4 = linear address A23:A20; 0 turns
     * the linear aperture off (GD542x TRM). The value each Zorro board
     * needs is the one Linux cirrusfb (drivers/video/fbdev/cirrusfb.c)
     * programs: Picasso II/IV 0x2x, Piccolo/Spectrum 0x8x, SD64 0xFx. */
    lin = 0x20;
    if (c->kind == K_PICCOLO)
        lin = (c->ids[0] == 0xa8) ? 0xf0 : 0x80;
    else if (c->kind == K_PIXEL64)
        lin = 0x80;
    SR_W(7, (UBYTE)((cst.sr[7] & 0x0f) | lin));
    GR_W(0x09, 0);
    GR_W(0x0a, 0);

    vga_std_tests();

    {
        static const struct regm sr_ext[] = { {0x10, 0xff}, {0x11, 0xff}, {0x12, 0x01}, {0x13, 0x3f} };
        static const struct regm gr_ext[] = { {0x09, 0xff}, {0x0a, 0xff}, {0x10, 0xff}, {0x11, 0xff} };
        static const struct regm gr_blt[] = {
            {0x20, 0xff}, {0x21, 0x07}, {0x22, 0xff}, {0x23, 0x03}, {0x24, 0xff}, {0x25, 0x0f},
            {0x26, 0xff}, {0x27, 0x0f}, {0x28, 0xff}, {0x29, 0xff}, {0x2a, 0x1f}, {0x2c, 0xff},
            {0x2d, 0xff}, {0x2e, 0x1f}, {0x32, 0xff} };
        static const struct regm cr_ext[] = { {0x19, 0xff} };
        rtg_regfile("cl_cursor_regs", 0x3c4, sr_ext, 4, "hardware cursor register (SR10/SR11 position, SR12 bit 0 enable, SR13 pattern 5:0) does not read back (GD542x TRM)");
        rtg_regfile("cl_gr_ext", 0x3ce, gr_ext, 4, "extended graphics register (GR9/GRA offset/bank, GR10/GR11 colour expansion bytes) does not read back");
        rtg_regfile("cl_blt_regs", 0x3ce, gr_blt, 15, "BitBLT register (GR20-GR2E width/height/pitch/addresses, GR32 ROP) does not read back (masks: GD542x minimum widths)");
        rtg_regfile("cl_cr_ext", 0x3d4, cr_ext, 1, "CR19 (interlace end) does not read back");
    }

    /* mode set: 640x480, 8 bits per pixel packed (SR7 bit 0), extended
     * addressing (CR1B bit 1) */
    vga_set_640x480();
    SR_W(7, (UBYTE)(lin | 0x01));
    CR_W(0x1b, (UBYTE)((cst.cr[0x1b] & ~0x1d) | 0x02));
    vga_check_640x480("mode_640x480");
    rtg_chk("cl_mode_sr7", "sr7", lin | 0x01, SR_R(7), "SR7 (linear address 7:4, extended sequencer mode bit 0) does not read back after the mode set");

    /* VRAM through the linear aperture in the packed 8-bit mode (in the
     * planar modes CPU writes go through the VGA write logic) */
    for (i = 0; i < (int)(RTG_WORK / 4); i++)
        ((ULONG *)rtg_save)[i] = ((volatile ULONG *)vram)[i];
    rtg_vram_tests();
    vga_palette_load();
    rtg_draw_test("mode_draw", PITCH, 100);
    vga_retrace_test();
    rtg_screenshot("mode_screenshot", rtg_cid("640x480"));

    /* 2D engine */
    blt_rops();
    blt_pattern();
    blt_cexp(0, 0);
    blt_cexp(1, 0);
    blt_cexp(0, 1);
    blt_backward();
    if ((id & 0xfc) >= 0xac)
        blt_autostart();

    /* vertical interrupt */
    vga_vint_test(c->kind == K_PICASSO4_Z2 || c->kind == K_PICASSO4_Z3 ? cl_noop_irq :
                  (c->irq ? cl_board_irq : 0));

    /* restore: VRAM work area, extension registers, standard VGA, lock */
    for (i = 0; i < (int)(RTG_WORK / 4); i++)
        ((volatile ULONG *)vram)[i] = ((ULONG *)rtg_save)[i];
    for (i = 0x09; i <= 0x0b; i++) GR_W(i, cst.gr[i]);
    for (i = 0x10; i <= 0x11; i++) GR_W(i, cst.gr[i]);
    for (i = 0x20; i <= 0x2e; i++) if (i != 0x2b) GR_W(i, cst.gr[i]);
    GR_W(0x30, cst.gr[0x30]);
    GR_W(0x32, cst.gr[0x32]);
    GR_W(0x31, (UBYTE)(gr31_pre & 0x80));
    SR_W(0, 0x01);
    SR_W(7, cst.sr[7]);
    for (i = 0x10; i <= 0x13; i++) SR_W(i, cst.sr[i]);
    SR_W(0, 0x03);
    CR_W(0x19, cst.cr[0x19]);
    CR_W(0x1b, cst.cr[0x1b]);
    vga_restore_std(&cst);
    SR_W(0, cst.sr[0]);
    SR_W(6, sr6 == 0x12 ? 0x12 : 0x00);
    if (c->kind == K_PICASSO4_Z2)
        c->regs[0] = p4_bank;
}
