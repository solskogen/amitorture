/* rtg: S3 Trio64 (CyberVision 64) and S3 ViRGE (CyberVision 64/3D) suite.
 * References: S3 Trio32/Trio64 data book (CR2D/2E/30 ids, register locks
 * CR38/CR39, enhanced 8514/A-compatible graphics engine, hardware cursor
 * CR45-CR4F), S3 ViRGE data book (S3d engine MMIO), IBM VGA TR. */
#include "rtg.h"

static struct vga_state sst;
static UBYTE s3_saved[0x50];   /* CR30-CR6F as found (index - 0x30) */
static ULONG cv64_ctl_now;
static int virge_retry = -1;
static volatile ULONG *virge_pci;   /* ViRGE PCI config space, 0 if none */
static ULONG virge_cmd;             /* its command/status register as found */   /* -1 not tried, 1 a second identical command worked, 0 not */

#define S3_PITCH 640

/* ---- board control (CV64 Z3: write-only byte at board + 0x40001; bits
 * 5/6 select the VRAM window byte order, 4 the monitor switch, 3 the
 * interrupt enable, 7 the level). Assumed 0 at power-up (NetBSD grf_cv
 * clears all bits at init). */
static void cv64_ctl(UBYTE v)
{
    cv64_ctl_now = v;
    rtg_cur->mem[0x40001] = v;
}

/* ---- 8514/A-compatible engine registers, written as two byte accesses
 * (low byte at the even port, high byte at port + 1), which the S3 ports
 * decode; no assumption about how the board maps 16-bit cycles. */
static void ew(UWORD port, UWORD v)
{
    vo(port, (UBYTE)v);
    vo(port + 1, (UBYTE)(v >> 8));
}

static UWORD er(UWORD port)
{
    return (UWORD)(vi(port) | ((UWORD)vi(port + 1) << 8));
}

static int s3_idle(void)
{
    ULONG f0 = amt_frames_now();
    while (er(0x9ae8) & 0x0200) {           /* GP_STAT bit 9: engine busy */
        if (amt_frames_now() - f0 > 25)
            return 0;
    }
    return 1;
}

static UBYTE mix8(UBYTE mix, UBYTE s, UBYTE d)
{
    switch (mix & 15) {
    case 0x0: return ~d;
    case 0x1: return 0;
    case 0x2: return 0xff;
    case 0x3: return d;
    case 0x4: return ~s;
    case 0x5: return s ^ d;
    case 0x6: return ~(s ^ d);
    case 0x7: return s;
    case 0x8: return ~(s & d);
    case 0x9: return ~s | d;
    case 0xa: return s | ~d;
    case 0xb: return s | d;
    case 0xc: return s & d;
    case 0xd: return ~s & d;
    case 0xe: return s & ~d;
    default:  return ~s & ~d;
    }
}

static void fill_rand(ULONG a, int w, int h)
{
    volatile UBYTE *p = rtg_cur->win[0].p;
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            p[a + (ULONG)y * S3_PITCH + x] = (UBYTE)(amt_rand() >> 13);
}

static void grab(UBYTE *buf, ULONG d, int w, int h)
{
    volatile UBYTE *p = rtg_cur->win[0].p;
    int x, y, i = 0;
    for (y = -1; y <= h; y++)
        for (x = -1; x <= w; x++)
            buf[i++] = p[(LONG)d + (LONG)y * S3_PITCH + x];
}

static void report(const char *suffix, const char *why, UBYTE *exp, UBYTE *got, int w, int h,
                   int done, ULONG cmd, ULONG rop, int sx, int sy, int dx, int dy)
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
    json_str("why", done ? why : "graphics engine still busy after 25 frames");
    json_obj("in");
    json_hex("cmd", cmd); json_hex("rop", rop);
    json_int("sx", sx); json_int("sy", sy); json_int("dx", dx); json_int("dy", dy);
    json_int("w", w); json_int("h", h); json_int("pitch", S3_PITCH);
    if (bad >= 0) { json_int("x", bad % (w + 2) - 1); json_int("y", bad / (w + 2) - 1); }
    json_obj_end();
    json_obj("diff"); json_int("bytes", (long)nbad); json_hex("rows", rows); json_str("rows_is", "bit r = region row r (row 0 = border above)"); json_obj_end();
    json_obj("ctx"); json_str("region", "dest rect plus a 1-pixel border, which must stay unchanged");
    if (virge_retry >= 0) json_int("same_command_again_ok", virge_retry);
    rtg_ctx_card(); json_obj_end();
    amt_case_close();
}

/* ---------------------------------------------------- Trio64 engine */
static void trio_common(void)
{
    ew(0xbee8, 0xa000);       /* PIX_CNTL: foreground mix always */
    ew(0xaae8, 0xffff);       /* WRT_MASK */
    ew(0xaee8, 0xffff);       /* RD_MASK */
    ew(0xbee8, 0x1000);       /* scissors top 0 */
    ew(0xbee8, 0x2000);       /* left 0 */
    ew(0xbee8, 0x3000 | 479); /* bottom */
    ew(0xbee8, 0x4000 | 639); /* right */
}

static void trio_blt_mixes(void)
{
    static const UBYTE mixes[8] = { 0x7, 0x5, 0xc, 0xb, 0x4, 0x1, 0x2, 0x0 };
    UBYTE *before = rtg_buf, *exp = rtg_buf + 4096, *got = rtg_buf + 8192;
    UBYTE srcb[40 * 12];
    const int w = 40, h = 12, sx = 8, sy = 4, dx = 320, dy = 40;
    const ULONG src = (ULONG)sy * S3_PITCH + sx, dst = (ULONG)dy * S3_PITCH + dx;
    volatile UBYTE *p = rtg_cur->win[0].p;
    int m, x, y, done;
    char suf[24];
    for (m = 0; m < 8; m++) {
        UBYTE mix = mixes[m];
        char *d = suf;
        const char *s = "s3_blt_mix_";
        while (*s) *d++ = *s++;
        *d++ = "0123456789abcdef"[mix]; *d = 0;
        fill_rand(src, w, h);
        fill_rand(dst - S3_PITCH - 1, w + 2, h + 2);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                srcb[y * w + x] = p[src + (ULONG)y * S3_PITCH + x];
        grab(before, dst, w, h);
        for (x = 0; x < (w + 2) * (h + 2); x++) exp[x] = before[x];
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int i = (y + 1) * (w + 2) + x + 1;
                exp[i] = mix8(mix, srcb[y * w + x], before[i]);
            }
        s3_idle();
        trio_common();
        ew(0xbae8, (UWORD)(0x60 | mix));  /* FRGD_MIX: source = display memory */
        ew(0x86e8, sx); ew(0x82e8, sy);
        ew(0x8ee8, dx); ew(0x8ae8, dy);
        ew(0x96e8, w - 1);
        ew(0xbee8, (UWORD)(0x0000 | (h - 1)));
        ew(0x9ae8, 0xc0b1);               /* BitBLT, draw, +X, +Y, write */
        done = s3_idle();
        grab(got, dst, w, h);
        report(suf, "screen-to-screen BitBLT (8514/A command 110) with this foreground mix differs from the CPU result (S3 Trio64 data book mix table)",
               exp, got, w, h, done, 0xc0b1, mix, sx, sy, dx, dy);
    }
}

static void trio_rectfill(void)
{
    UBYTE *before = rtg_buf, *exp = rtg_buf + 4096, *got = rtg_buf + 8192;
    const int w = 37, h = 9, dx = 101, dy = 70;
    const ULONG dst = (ULONG)dy * S3_PITCH + dx;
    const UBYTE col = 0xa6;
    int x, y, done;
    fill_rand(dst - S3_PITCH - 1, w + 2, h + 2);
    grab(before, dst, w, h);
    for (x = 0; x < (w + 2) * (h + 2); x++) exp[x] = before[x];
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            exp[(y + 1) * (w + 2) + x + 1] = mix8(0x5, col, before[(y + 1) * (w + 2) + x + 1]);
    s3_idle();
    trio_common();
    ew(0xa6e8, col);                      /* FRGD_COLOR */
    ew(0xbae8, 0x25);                     /* FRGD_MIX: foreground colour, XOR */
    ew(0x86e8, dx); ew(0x82e8, dy);
    ew(0x96e8, w - 1);
    ew(0xbee8, (UWORD)(h - 1));
    ew(0x9ae8, 0x40b1);                   /* rectangle fill */
    done = s3_idle();
    grab(got, dst, w, h);
    report("s3_rectfill", "rectangle fill (command 010) with FRGD_COLOR and mix XOR differs from the CPU result",
           exp, got, w, h, done, 0x40b1, 0x25, 0, 0, dx, dy);
}

/* lines whose pixel set does not depend on Bresenham details:
 * horizontal, vertical, 45 degree; last pixel drawn */
static void trio_lines(void)
{
    UBYTE *before = rtg_buf, *exp = rtg_buf + 4096, *got = rtg_buf + 8192;
    const int w = 24, h = 24, bx = 200, by = 10;
    const ULONG box = (ULONG)by * S3_PITCH + bx;
    int t, x, done = 1;
    const UBYTE col[3] = { 0x31, 0x52, 0x73 };
    fill_rand(box - S3_PITCH - 1, w + 2, h + 2);
    grab(before, box, w, h);
    for (x = 0; x < (w + 2) * (h + 2); x++) exp[x] = before[x];
    for (t = 0; t < 3; t++) {
        /* box-relative: t0 (2..22, 2), t1 (12, 3..23), t2 (2+i, 1+i) */
        int len = 20, i;
        UWORD cmd;
        for (i = 0; i <= len; i++) {
            int px = t == 0 ? 2 + i : (t == 1 ? 12 : 2 + i);
            int py = t == 0 ? 2 : (t == 1 ? 3 + i : 1 + i);
            exp[(py + 1) * (w + 2) + px + 1] = col[t];
        }
        s3_idle();
        trio_common();
        ew(0xa6e8, col[t]);
        ew(0xbae8, 0x27);                 /* foreground colour, "new" */
        if (t == 0) {          /* +X major, dy = 0 */
            ew(0x86e8, bx + 2); ew(0x82e8, by + 2);
            ew(0x96e8, len); ew(0x8ae8, 0); ew(0x8ee8, (UWORD)(-2 * len)); ew(0x92e8, (UWORD)(-len));
            cmd = 0x20b1;
        } else if (t == 1) {   /* +Y major, dx = 0 */
            ew(0x86e8, bx + 12); ew(0x82e8, by + 3);
            ew(0x96e8, len); ew(0x8ae8, 0); ew(0x8ee8, (UWORD)(-2 * len)); ew(0x92e8, (UWORD)(-len));
            cmd = 0x20f1;
        } else {               /* 45 degrees: dx = dy */
            ew(0x86e8, bx + 2); ew(0x82e8, by + 1);
            ew(0x96e8, len); ew(0x8ae8, (UWORD)(2 * len)); ew(0x8ee8, 0); ew(0x92e8, (UWORD)len);
            cmd = 0x20b1;
        }
        ew(0x9ae8, cmd);
        if (!s3_idle()) done = 0;
    }
    grab(got, box, w, h);
    report("s3_line", "line draw (command 001: horizontal, vertical, 45 degree lines, last pixel on) differs from the expected pixel set",
           exp, got, w, h, done, 0x20b1, 0x27, 0, 0, bx, by);
}

/* ---------------------------------------------------- ViRGE S3d engine */
static volatile ULONG *virge_mmio(void)
{
    if (rtg_cur->kind == K_CV643D_Z3)
        return (volatile ULONG *)(rtg_cur->mem + 0x5000000);
    return 0;
}

static int virge_idle(volatile ULONG *m)
{
    ULONG f0 = amt_frames_now();
    /* SUBSYS_STAT: bit 13 S3d engine idle, bits 12:8 free FIFO slots
     * (16 = empty); idle and empty as XFree86 s3virge WaitIdleEmpty */
    while ((m[0x8504 / 4] & 0x3f00) != 0x3000) {
        if (amt_frames_now() - f0 > 25)
            return 0;
    }
    return 1;
}

/* MS ROP3 with S and D only (the ROPs used have equal nibbles, so the
 * pattern bit does not matter): result bit = rop bit (S << 1 | D) */
static UBYTE rop3(UBYTE r, UBYTE s, UBYTE d)
{
    UBYTE o = 0;
    int b;
    for (b = 0; b < 8; b++) {
        int idx = (((s >> b) & 1) << 1) | ((d >> b) & 1);
        if ((r >> idx) & 1) o |= (UBYTE)(1 << b);
    }
    return o;
}

static void virge_blt(void)
{
    static const UBYTE rops[6] = { 0xcc, 0x66, 0x88, 0xee, 0x33, 0x55 };
    volatile ULONG *m = virge_mmio();
    UBYTE *before = rtg_buf, *exp = rtg_buf + 4096, *got = rtg_buf + 8192;
    UBYTE srcb[40 * 12];
    const int w = 40, h = 12, sx = 8, sy = 4, dx = 320, dy = 40;
    const ULONG src = (ULONG)sy * S3_PITCH + sx, dst = (ULONG)dy * S3_PITCH + dx;
    volatile UBYTE *p = rtg_cur->win[0].p;
    int r, x, y, done;
    char suf[24];
    if (!m) {
        rtg_skip("virge_blt", "S3d engine MMIO window of this board layout not known to the module");
        return;
    }
    for (r = 0; r < 6; r++) {
        UBYTE rop = rops[r];
        ULONG cmd;
        char *d = suf;
        const char *s = "virge_blt_rop_";
        while (*s) *d++ = *s++;
        *d++ = "0123456789abcdef"[rop >> 4]; *d++ = "0123456789abcdef"[rop & 15]; *d = 0;
        fill_rand(src, w, h);
        fill_rand(dst - S3_PITCH - 1, w + 2, h + 2);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                srcb[y * w + x] = p[src + (ULONG)y * S3_PITCH + x];
        grab(before, dst, w, h);
        for (x = 0; x < (w + 2) * (h + 2); x++) exp[x] = before[x];
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int i = (y + 1) * (w + 2) + x + 1;
                exp[i] = rop3(rop, srcb[y * w + x], before[i]);
            }
        virge_idle(m);
        m[0xa4d4 / 4] = 0;                       /* source base */
        m[0xa4d8 / 4] = 0;                       /* destination base */
        m[0xa4dc / 4] = (0UL << 16) | 639;       /* clip left/right */
        m[0xa4e0 / 4] = (0UL << 16) | 479;       /* clip top/bottom */
        m[0xa4e4 / 4] = ((ULONG)S3_PITCH << 16) | S3_PITCH; /* dest/src stride */
        cmd = 0x00000020UL | ((ULONG)rop << 17) | (1UL << 25) | (1UL << 26); /* BitBLT, 8 bpp, draw, +X +Y */
        m[0xa500 / 4] = cmd;
        m[0xa504 / 4] = ((ULONG)(w - 1) << 16) | (ULONG)h;
        m[0xa508 / 4] = ((ULONG)sx << 16) | (ULONG)sy;
        m[0xa50c / 4] = ((ULONG)dx << 16) | (ULONG)dy;   /* starts the operation */
        done = virge_idle(m);
        grab(got, dst, w, h);
        /* diagnosis only: when the result is wrong, issue the identical
         * command once more and note whether that one worked */
        virge_retry = -1;
        for (x = 0; x < (w + 2) * (h + 2); x++)
            if (got[x] != exp[x]) {
                UBYTE *g2 = rtg_buf + 12288;
                int k;
                m[0xa500 / 4] = cmd;
                m[0xa504 / 4] = ((ULONG)(w - 1) << 16) | (ULONG)h;
                m[0xa508 / 4] = ((ULONG)sx << 16) | (ULONG)sy;
                m[0xa50c / 4] = ((ULONG)dx << 16) | (ULONG)dy;
                virge_idle(m);
                grab(g2, dst, w, h);
                virge_retry = 1;
                for (k = 0; k < (w + 2) * (h + 2); k++)
                    if (g2[k] != exp[k]) { virge_retry = 0; break; }
                break;
            }
        report(suf, "S3d BitBLT (command 0, 8 bpp) with this ROP3 differs from the CPU result (ViRGE data book; MS ROP3 with S,D)",
               exp, got, w, h, done, cmd, rop, sx, sy, dx, dy);
    }
}

/* ---------------------------------------------------- byte order modes of the CV64 window */
static ULONG lane(volatile UBYTE *nat, ULONG o, UBYTE msk, int width)
{
    ULONG v = 0;
    int i;
    for (i = 0; i < width; i++) v = (v << 8) | nat[(o + i) ^ msk];
    return v;
}

static void cv64_swap(UBYTE bits, UBYTE msk, const char *suffix)
{
    volatile UBYTE *p = rtg_cur->win[0].p;
    const ULONG o = 0x2340;
    ULONG save0 = *(volatile ULONG *)(p + o), save1 = *(volatile ULONG *)(p + o + 4);
    ULONG gl, gw, el, ew_, wl;
    UBYTE nat[8];
    int i;
    *(volatile ULONG *)(p + o) = 0x01234567UL;
    *(volatile ULONG *)(p + o + 4) = 0x89abcdefUL;
    for (i = 0; i < 8; i++) nat[i] = p[o + i];
    cv64_ctl((UBYTE)((cv64_ctl_now & ~0x60) | bits));
    gl = *(volatile ULONG *)(p + o);
    gw = *(volatile UWORD *)(p + o + 6);
    *(volatile ULONG *)(p + o) = 0xdeadbeefUL;
    cv64_ctl((UBYTE)(cv64_ctl_now & ~0x60));
    wl = lane(p, o, msk, 4);
    el = lane((volatile UBYTE *)nat, 0, msk, 4);
    ew_ = lane((volatile UBYTE *)nat, 6, msk, 2);
    *(volatile ULONG *)(p + o) = save0;
    *(volatile ULONG *)(p + o + 4) = save1;
    if (gl == el && gw == ew_ && wl == 0xdeadbeefUL) {
        rtg_pass(suffix);
        return;
    }
    amt_case_open(rtg_cid(suffix), AMT_ST_FAIL);
    json_obj("expect"); json_hex("long", el); json_hexn("word", ew_, 4); json_hex("written_back", 0xdeadbeefUL); json_obj_end();
    json_obj("got"); json_hex("long", gl); json_hexn("word", gw, 4); json_hex("written_back", wl); json_obj_end();
    json_str("why", "with the board's byte-order bits set, 16/32-bit VRAM accesses must see the natural bytes lane-swapped (mask 1 = bytes swapped in words, 3 = in longs)");
    json_obj("in"); json_hexn("ctl_bits", bits, 2); json_int("mask", msk); json_hex("offset", o); json_obj_end();
    json_obj("ctx"); rtg_ctx_card(); json_obj_end();
    amt_case_close();
}

/* ---------------------------------------------------- suite */
/* the vertical interrupt reaches the bus only with CR32 bit 4 (interrupt
 * enable, S3 data books), plus the board's own enable */
static void s3_board_irq(int on)
{
    CR_W(0x32, (UBYTE)(on ? (s3_saved[0x02] | 0x10) : s3_saved[0x02]));
    rtg_board_irq(on);
}

void rtg_s3(void)
{
    struct rtg_card *c = rtg_cur;
    int i, virge = c->chip == CH_S3VIRGE;
    UBYTE m0, id30, id2d, id2e, e2d = virge ? 0x56 : 0x88, e2e = virge ? 0x31 : 0x11;
    volatile UBYTE *vram;

    if (c->kind == K_CV643D_Z2 || c->kind == K_CV643D_Z3) {
        /* PCI configuration space of the ViRGE: vendor 0x5333, device 0x5631;
         * one long write to it hands the window over to the chip's I/O */
        volatile ULONG *pci = (volatile ULONG *)(c->kind == K_CV643D_Z2 ? c->mem + 0x3c0000 : c->mem + 0xc0e0000);
        ULONG idr = pci[0], cmd = pci[1];
        virge_pci = pci;
        virge_cmd = cmd;
        rtg_chk("virge_pci_id", "id", 0x56315333UL, idr, "PCI configuration register 0 of the ViRGE must read device 0x5631, vendor 0x5333 (S3 ViRGE data book)");
        pci[1] = (cmd & 0xffff0000UL) | 0x0003;   /* I/O + memory decode on */
        if (c->kind == K_CV643D_Z2)
            *(volatile UWORD *)(c->mem + 0x3a0008) = 0x0080;  /* VRAM window big-endian (board model) */
    }
    if (c->kind == K_CV64)
        cv64_ctl(0);

    m0 = vi(0x3cc);
    vga_color_io();
    for (i = 0; i < 2; i++) s3_saved[0x08 + i] = CR_R(0x38 + i);
    CR_W(0x38, 0x48);           /* unlock S3 VGA registers (CR30-CR3F) */
    CR_W(0x39, 0xa5);           /* unlock system control/extension registers (CR40-) */
    for (i = 0; i < 0x40; i++)
        if (i != 0x08 && i != 0x09) s3_saved[i] = CR_R(0x30 + i);
    vga_save(&sst, 0x20, 0x09, 0x30);
    sst.misc = m0;

    id30 = CR_R(0x30);
    id2d = CR_R(0x2d);
    id2e = CR_R(0x2e);
    if (id30 == 0xe1 && id2d == e2d && id2e == e2e) {
        amt_case_open(rtg_cid("s3_chip_id"), AMT_ST_PASS);
        json_obj("ctx"); json_hexn("cr2f", CR_R(0x2f), 2); json_obj_end();
        amt_case_close();
    } else {
        amt_case_open(rtg_cid("s3_chip_id"), AMT_ST_FAIL);
        json_obj("expect"); json_hexn("cr30", 0xe1, 2); json_hexn("cr2d", e2d, 2); json_hexn("cr2e", e2e, 2); json_obj_end();
        json_obj("got"); json_hexn("cr30", id30, 2); json_hexn("cr2d", id2d, 2); json_hexn("cr2e", id2e, 2); json_obj_end();
        json_str("why", virge ? "ViRGE: CR30 = 0xE1, CR2D:CR2E = 0x56:0x31 after CR38 = 0x48 (S3 ViRGE data book)"
                              : "Trio64: CR30 = 0xE1, CR2D:CR2E = 0x88:0x11 after CR38 = 0x48 (S3 Trio64 data book)");
        json_obj("ctx"); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }

    vga_std_tests();
    {
        static const struct regm cur[] = { {0x46, 0x07}, {0x47, 0xff}, {0x48, 0x07}, {0x49, 0xff},
                                           {0x4c, 0x03}, {0x4d, 0xff}, {0x4e, 0x3f}, {0x4f, 0x3f} };
        static const struct regm ext[] = { {0x50, 0xf1}, {0x69, 0x0f} };
        rtg_regfile("s3_cursor_regs", 0x3d4, cur, 8, "hardware cursor register (CR46/47 X, CR48/49 Y, CR4C/4D start address, CR4E/4F pattern offset) does not read back (S3 data book)");
        rtg_regfile("s3_ext_regs", 0x3d4, ext, 2, "extended system control register (CR50 screen width/pixel length, CR69 start address high) does not read back");
        /* CR45 bit 0: cursor enable */
        {
            UBYTE o = CR_R(0x45), r;
            CR_W(0x45, o | 1); r = CR_R(0x45) & 1;
            CR_W(0x45, o & ~1);
            r |= (UBYTE)((CR_R(0x45) & 1) << 1);
            CR_W(0x45, o);
            rtg_chk("s3_cursor_enable", "bits", 0x01, r, "CR45 bit 0 (hardware cursor enable) does not read back as set then cleared");
        }
    }

    /* mode: 640x480 8 bpp enhanced (CR31 bit 3, CR3A bit 4), engine
     * width 640 (CR50 = 0x40), linear window 4 MB on (CR58 = 0x13) */
    vga_set_640x480();
    CR_W(0x31, (UBYTE)((s3_saved[0x01] & ~0x08) | 0x08));
    CR_W(0x3a, (UBYTE)((s3_saved[0x0a] & ~0x10) | 0x10));
    CR_W(0x67, 0x00);
    if (virge) {
        /* linear window 4 MB on (CR58), base 0 (the board decodes the
         * Zorro window), new MMIO at base + 16 MB on (CR53 bit 3) */
        CR_W(0x59, 0x00);
        CR_W(0x5a, 0x00);
        CR_W(0x58, (UBYTE)((s3_saved[0x28] & ~0x17) | 0x13));
        CR_W(0x53, (UBYTE)(s3_saved[0x23] | 0x08));
    } else {
        CR_W(0x50, 0x40);
        CR_W(0x59, 0x01);
        CR_W(0x5a, 0x40);
        CR_W(0x58, (UBYTE)((s3_saved[0x28] & ~0x13) | 0x13));
        CR_W(0x40, (UBYTE)(s3_saved[0x10] | 0x01));   /* enhanced register access */
    }
    vga_check_640x480("mode_640x480");
    vga_palette_load();

    vram = c->win[0].p;
    for (i = 0; i < (int)(RTG_WORK / 4); i++)
        ((ULONG *)rtg_save)[i] = ((volatile ULONG *)vram)[i];
    rtg_vram_tests();
    if (c->kind == K_CV64) {
        cv64_swap(0x20, 1, "vram_wordswap");
        cv64_swap(0x40, 3, "vram_longswap");
    }
    rtg_draw_test("mode_draw", S3_PITCH, 100);
    vga_retrace_test();
    rtg_screenshot("mode_screenshot", rtg_cid("640x480"));

    if (!virge) {
        trio_blt_mixes();
        trio_rectfill();
        trio_lines();
    } else
        virge_blt();

    vga_vint_test(s3_board_irq);

    /* restore */
    s3_idle();
    for (i = 0; i < (int)(RTG_WORK / 4); i++)
        ((volatile ULONG *)vram)[i] = ((ULONG *)rtg_save)[i];
    vga_restore_std(&sst);
    vo(0x3c2, sst.misc | 1);
    if (virge) CR_W(0x53, s3_saved[0x23]);
    {
        if (!virge) CR_W(0x40, s3_saved[0x10]);
        CR_W(0x58, s3_saved[0x28]);
        CR_W(0x59, s3_saved[0x29]);
        CR_W(0x5a, s3_saved[0x2a]);
    }
    CR_W(0x67, s3_saved[0x37]);
    CR_W(0x50, s3_saved[0x20]);
    CR_W(0x69, s3_saved[0x39]);
    CR_W(0x45, s3_saved[0x15]);
    for (i = 0x46; i <= 0x4f; i++) CR_W(i, s3_saved[i - 0x30]);
    CR_W(0x3a, s3_saved[0x0a]);
    CR_W(0x31, s3_saved[0x01]);
    CR_W(0x39, s3_saved[0x09]);
    CR_W(0x38, s3_saved[0x08]);
    vo(0x3c2, sst.misc);
    if (c->kind == K_CV64)
        cv64_ctl(0);
    if (c->kind == K_CV643D_Z2)
        *(volatile UWORD *)(c->mem + 0x3a0008) = 0x0000;
    /* PCI command register as found (review M12); +$3A0008 above is write
     * only, 0 is the board's reset value */
    if (virge_pci) {
        virge_pci[1] = virge_cmd;
        virge_pci = 0;
    }
}
