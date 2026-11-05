/* rtg: graphics cards, register level (CONTRACT.md section 10, id 16).
 *
 * For every board in amt_probe_result.boards whose manufacturer/product
 * this module recognises (it never writes to an unknown board), it runs:
 *   - VRAM: B/W/L consistency, address lines, a sample per 64 KB over the
 *     window (size and mirroring), byte-swapped windows against the natural
 *     window;
 *   - the chip's register file: VGA sequencer/graphics/CRTC/attribute
 *     index-data read-back, the DAC, chip id and extension registers;
 *   - a 640x480 8-bit mode set, a drawn pattern read back, a screenshot
 *     request (hash informational, host side);
 *   - the 2D engine with an exact oracle: the same operation done by the
 *     CPU on a RAM copy, compared byte for byte;
 *   - hardware cursor registers, vertical retrace status and the vertical
 *     interrupt (chip level, and at the Amiga level where the board has one);
 * and restores the readable configuration, DAC and work VRAM. Execution
 * triggers, internal pointers and write-only board state are not generally
 * restorable; see docs/cases/rtg.md for ownership gates and limitations.
 *
 * The chip-level expectations come from the VGA standard (IBM VGA technical
 * reference) and the chip manuals (Cirrus GD542x/543x/5446 TRM, S3 Trio64
 * and ViRGE data books, Tseng ET4000 data book, TI TMS34010 user's guide).
 * Board-level details (where a Zorro board puts the chip's I/O ports, its
 * monitor switch) have no public datasheet for most boards; they are taken
 * from the Picasso96 driver behaviour as modelled by WinUAE/Amiberry, and
 * are only used as access paths, never as expectations (see
 * docs/cases/rtg.md).
 */
#include "rtg.h"
#include "cia_hw.h"      /* CIA save/restore (shared header of the cia module) */

AMT_MODULE(rtg, 16, AMT_PH_BARE, AMT_MF_LONG, 0, 65536 + 16384);

struct rtg_card *rtg_cur;
UBYTE *rtg_save;
UBYTE *rtg_buf;

static struct rtg_card cards[8];
static int ncards;

/* ------------------------------------------------------------ reporting */
static char cidbuf[64];

const char *rtg_cid(const char *suffix)
{
    char *d = cidbuf;
    const char *s = rtg_cur ? rtg_cur->id : "rtg";
    while (*s && d < cidbuf + 30)
        *d++ = *s++;
    *d++ = '_';
    s = suffix;
    while (*s && d < cidbuf + sizeof cidbuf - 1)
        *d++ = *s++;
    *d = 0;
    return cidbuf;
}

void rtg_pass(const char *suffix) { amt_case(rtg_cid(suffix), AMT_ST_PASS, 0); }
void rtg_skip(const char *suffix, const char *why) { amt_case(rtg_cid(suffix), AMT_ST_SKIP, why); }
void rtg_untestable(const char *suffix, const char *why) { amt_case(rtg_cid(suffix), AMT_ST_UNTESTABLE, why); }

void rtg_ctx_card(void)
{
    struct rtg_card *c = rtg_cur;
    json_obj("card");
    json_str("name", c->name);
    json_int("man", (long)c->man);
    json_int("prod", (long)c->prod_mem);
    if (c->mem)
        json_hex("mem", (ULONG)c->mem);
    if (c->regs)
        json_hex("regs", (ULONG)c->regs);
    json_obj_end();
}

int rtg_chk(const char *suffix, const char *what, ULONG expect, ULONG got, const char *why)
{
    if (expect == got) {
        rtg_pass(suffix);
        return 1;
    }
    amt_case_open(rtg_cid(suffix), AMT_ST_FAIL);
    json_obj("expect"); json_hex(what, expect); json_obj_end();
    json_obj("got"); json_hex(what, got); json_obj_end();
    json_str("why", why);
    json_obj("ctx"); rtg_ctx_card(); json_obj_end();
    amt_case_close();
    return 0;
}

/* ------------------------------------------------------------ register files */
static const UBYTE tvals[5] = { 0x55, 0xaa, 0x00, 0xff, 0x69 };

int rtg_regfile(const char *suffix, UWORD port, const struct regm *l, int n, const char *why)
{
    int i, k, bad = 0;
    UBYTE bidx = 0, bw = 0, br = 0, bm = 0;
    for (i = 0; i < n; i++) {
        UBYTE orig = ix_r(port, l[i].idx);
        for (k = 0; k < 5; k++) {
            UBYTE w = (UBYTE)((orig & ~l[i].mask) | (tvals[k] & l[i].mask)), r;
            ix_w(port, l[i].idx, w);
            r = ix_r(port, l[i].idx);
            if ((r ^ w) & l[i].mask) {
                if (!bad) { bidx = l[i].idx; bw = w; br = r; bm = l[i].mask; }
                bad++;
                break;
            }
        }
        ix_w(port, l[i].idx, orig);
    }
    if (!bad) {
        rtg_pass(suffix);
        return 1;
    }
    amt_case_open(rtg_cid(suffix), AMT_ST_FAIL);
    json_obj("expect"); json_hexn("value", bw & bm, 2); json_obj_end();
    json_obj("got"); json_hexn("value", br & bm, 2); json_obj_end();
    json_str("why", why);
    json_obj("in");
    json_hexn("port", port, 4); json_hexn("index", bidx, 2);
    json_hexn("written", bw, 2); json_hexn("mask", bm, 2);
    json_obj_end();
    json_obj("ctx"); json_int("bad_regs", bad); json_int("regs", n); rtg_ctx_card(); json_obj_end();
    amt_case_close();
    return 0;
}

/* ------------------------------------------------------------ VGA generic */
void vga_color_io(void)
{
    UBYTE m = vi(0x3cc);
    if (!(m & 1))
        vo(0x3c2, m | 1);
}

static UBYTE ar_r(UBYTE i)
{
    (void)vi((vi(0x3cc) & 1) ? 0x3da : 0x3ba);
    vo(0x3c0, i);
    return vi(0x3c1);
}

static void ar_w(UBYTE i, UBYTE v)
{
    (void)vi((vi(0x3cc) & 1) ? 0x3da : 0x3ba);
    vo(0x3c0, i);
    vo(0x3c0, v);
}

static void dac_read_all(UBYTE *d)
{
    int i;
    vo(DAC_RI, 0);
    for (i = 0; i < 768; i++)
        d[i] = vi(DAC_D);
}

static void dac_write_all(const UBYTE *d)
{
    int i;
    vo(DAC_WI, 0);
    for (i = 0; i < 768; i++)
        vo(DAC_D, d[i]);
}

void vga_save_selectors(struct vga_state *s)
{
    s->srx = vi(0x3c4);
    s->grx = vi(0x3ce);
    s->crx = vi(0x3d4);
    s->crmx = vi(0x3b4);
}

void vga_save(struct vga_state *s, int nsr, int ngr, int ncr)
{
    int i;
    s->misc = vi(0x3cc);
    s->feat = vi(0x3ca);
    s->pel = vi(DAC_M);
    (void)vi((vi(0x3cc) & 1) ? 0x3da : 0x3ba);
    s->arx = vi(0x3c0);           /* index register incl. PAS (bit 5) */
    for (i = 0; i < nsr && i < 0x20; i++) {
        s->sr[i] = SR_R(i);
        /* GD542x cursor bits 2:0 appear in the sequencer index readback,
         * bits 7:5, after selecting SR10/SR11. */
        if (rtg_cur->chip == CH_CIRRUS && i == 0x10) s->cursor_xlo = vi(0x3c4) & 0xe0;
        if (rtg_cur->chip == CH_CIRRUS && i == 0x11) s->cursor_ylo = vi(0x3c4) & 0xe0;
    }
    for (i = 0; i < ngr && i < 0x40; i++) s->gr[i] = GR_R(i);
    for (i = 0; i < ncr && i < 0x80; i++) s->cr[i] = CR_R(i);
    for (i = 0; i < 0x15; i++) s->ar[i] = ar_r(i);
    dac_read_all(s->dac);
    (void)vi((vi(0x3cc) & 1) ? 0x3da : 0x3ba);
    vo(0x3c0, s->arx);
}

void vga_restore_selectors(const struct vga_state *s)
{
    vo(0x3c4, s->srx);
    vo(0x3ce, s->grx);
    vo(0x3d4, s->crx);
    vo(0x3b4, s->crmx);
}

void vga_restore_std(const struct vga_state *s)
{
    int i;
    vo(0x3c2, s->misc | 1);       /* CRTC at 3Dx while restoring */
    for (i = 1; i < 5; i++) SR_W(i, s->sr[i]);
    for (i = 0; i < 9; i++) GR_W(i, s->gr[i]);
    CR_W(0x11, s->cr[0x11] & 0x7f);
    for (i = 0; i < 0x19; i++)
        if (i != 0x11) CR_W(i, s->cr[i]);
    CR_W(0x11, s->cr[0x11]);
    for (i = 0; i < 0x15; i++) ar_w(i, s->ar[i]);
    dac_write_all(s->dac);
    vo(DAC_M, s->pel);
    (void)vi(0x3da);
    vo(0x3c0, s->arx & 0x3f);
    SR_W(0, s->sr[0]);
    vo(0x3c2, s->misc);
    vga_restore_selectors(s);
}

static const struct regm vga_sr[] = { {2, 0x0f}, {3, 0x3f} };
static const struct regm vga_gr[] = { {0, 0x0f}, {1, 0x0f}, {2, 0x0f}, {3, 0x1f}, {7, 0x0f}, {8, 0xff} };
static const struct regm vga_cr[] = { {0x0a, 0x3f}, {0x0b, 0x7f}, {0x0c, 0xff}, {0x0d, 0xff},
                                      {0x0e, 0xff}, {0x0f, 0xff}, {0x13, 0xff}, {0x14, 0x7f} };

static void idx_test(const char *suffix, UWORD port, UBYTE mask, const char *why)
{
    static const UBYTE iv[4] = { 0x05, 0x0a, 0x03, 0x0c };
    UBYTE orig = vi(port), r = 0, w = 0;
    int k, bad = 0;
    for (k = 0; k < 4; k++) {
        w = iv[k] & mask;
        vo(port, w);
        r = vi(port);
        if ((r & mask) != w) { bad = 1; break; }
    }
    vo(port, orig);
    if (!bad) { rtg_pass(suffix); return; }
    amt_case_open(rtg_cid(suffix), AMT_ST_FAIL);
    json_obj("expect"); json_hexn("index", w, 2); json_obj_end();
    json_obj("got"); json_hexn("index", r & mask, 2); json_obj_end();
    json_str("why", why);
    json_obj("in"); json_hexn("port", port, 4); json_hexn("mask", mask, 2); json_obj_end();
    json_obj("ctx"); rtg_ctx_card(); json_obj_end();
    amt_case_close();
}

static void attr_test(void)
{
    /* AR00-0F palette (6 bits), AR12 colour plane enable (bits 3:0),
     * AR13 pixel panning (3:0), AR14 colour select (3:0); IBM VGA TR */
    static const UBYTE idx[5] = { 0x00, 0x07, 0x0f, 0x13, 0x14 };
    static const UBYTE msk[5] = { 0x3f, 0x3f, 0x3f, 0x0f, 0x0f };
    int i, k, bad = 0;
    UBYTE bi = 0, bw = 0, br = 0, arx;
    (void)vi(0x3da);
    arx = vi(0x3c0);
    for (i = 0; i < 5 && !bad; i++) {
        UBYTE orig = ar_r(idx[i]);
        for (k = 0; k < 5; k++) {
            UBYTE w = (UBYTE)((orig & ~msk[i]) | (tvals[k] & msk[i])), r;
            ar_w(idx[i], w);
            r = ar_r(idx[i]);
            if ((r ^ w) & msk[i]) { bad = 1; bi = idx[i]; bw = w; br = r; break; }
        }
        ar_w(idx[i], orig);
    }
    /* the attribute index read-back at 3C0 (bits 4:0) after a write */
    if (!bad) {
        (void)vi(0x3da);
        vo(0x3c0, 0x12);
        br = vi(0x3c0);
        if ((br & 0x1f) != 0x12) { bad = 2; bi = 0xff; bw = 0x12; }
    }
    (void)vi(0x3da);
    vo(0x3c0, arx);
    if (!bad) { rtg_pass("vga_attr"); return; }
    amt_case_open(rtg_cid("vga_attr"), AMT_ST_FAIL);
    json_obj("expect"); json_hexn("value", bw, 2); json_obj_end();
    json_obj("got"); json_hexn("value", br, 2); json_obj_end();
    json_str("why", bad == 2 ? "attribute controller index (3C0 read, bits 4:0) does not read back the index written after the flip-flop reset (IBM VGA TR)"
                             : "attribute controller register does not read back through 3C1 after an index/data write pair at 3C0 (IBM VGA TR)");
    json_obj("in"); json_hexn("index", bi, 2); json_obj_end();
    json_obj("ctx"); rtg_ctx_card(); json_obj_end();
    amt_case_close();
}

static void dac_test(void)
{
    /* IBM VGA TR / Cirrus TRM / S3: write index 3C8, R,G,B to 3C9 (6 bits),
     * the index advances after the blue value; read index 3C7; 3C8 reads
     * back the write index; 3C7 reads the DAC state: 0 after a 3C8 write
     * (write mode), 3 after a 3C7 write (read mode). */
    UBYTE *exp = rtg_buf, *got = rtg_buf + 1024;
    int i, bad = -1;
    UBYTE wi, st_w, st_r;
    for (i = 0; i < 768; i++)
        exp[i] = (UBYTE)((amt_rand() >> 8) & 0x3f);
    vo(DAC_WI, 0);
    for (i = 0; i < 768; i++)
        vo(DAC_D, exp[i]);
    vo(DAC_RI, 0);
    for (i = 0; i < 768; i++)
        got[i] = vi(DAC_D) & 0x3f;
    for (i = 0; i < 768; i++)
        if (got[i] != exp[i]) { bad = i; break; }
    if (bad < 0)
        rtg_pass("dac_rw");
    else {
        amt_case_open(rtg_cid("dac_rw"), AMT_ST_FAIL);
        json_obj("expect"); json_hexn("value", exp[bad], 2); json_obj_end();
        json_obj("got"); json_hexn("value", got[bad], 2); json_obj_end();
        json_str("why", "palette DAC entry does not read back (3C8/3C9 write, 3C7/3C9 read, 6-bit values, auto-increment after B)");
        json_obj("in"); json_int("entry", bad / 3); json_int("component", bad % 3); json_obj_end();
        json_obj("ctx"); json_hex("crc_exp", crc32_update(0, exp, 768)); json_hex("crc_got", crc32_update(0, got, 768));
        rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
    /* write index read-back and auto-increment: 2 entries from 0x40
     * (VGA DAC semantics; a BT482 has no DAC state register) */
    if (rtg_cur->dac_bt482)
        rtg_skip("dac_index", "BT482 RAMDAC on this board: no VGA DAC state register (3C7 read)");
    else {
    vo(DAC_WI, 0x40);
    for (i = 0; i < 6; i++)
        vo(DAC_D, (UBYTE)(i + 1));
    wi = vi(DAC_WI);
    st_w = vi(DAC_RI) & 3;
    vo(DAC_RI, 0x40);
    st_r = vi(DAC_RI) & 3;
    {
        UBYTE r0 = vi(DAC_D) & 0x3f;
        if (wi == 0x42 && st_w == 0 && st_r == 3 && r0 == 1)
            rtg_pass("dac_index");
        else {
            amt_case_open(rtg_cid("dac_index"), AMT_ST_FAIL);
            json_obj("expect"); json_hexn("write_index", 0x42, 2); json_int("state_w", 0); json_int("state_r", 3);
            json_hexn("r40", 1, 2); json_obj_end();
            json_obj("got"); json_hexn("write_index", wi, 2); json_int("state_w", st_w); json_int("state_r", st_r);
            json_hexn("r40", r0, 2); json_obj_end();
            json_str("why", "3C8 must read back the write index (advanced by one per R,G,B triple), 3C7 must read the DAC state 0 in write mode and 3 in read mode (IBM VGA TR)");
            json_obj("ctx"); rtg_ctx_card(); json_obj_end();
            amt_case_close();
        }
    }
    }
    /* PEL mask 3C6: 8-bit read/write */
    {
        UBYTE o = vi(DAC_M), r1, r2;
        vo(DAC_M, 0x5a); r1 = vi(DAC_M);
        vo(DAC_M, 0xa5); r2 = vi(DAC_M);
        vo(DAC_M, o);
        if (r1 == 0x5a && r2 == 0xa5)
            rtg_pass("dac_pelmask");
        else {
            amt_case_open(rtg_cid("dac_pelmask"), AMT_ST_FAIL);
            json_obj("expect"); json_hexn("a", 0x5a, 2); json_hexn("b", 0xa5, 2); json_obj_end();
            json_obj("got"); json_hexn("a", r1, 2); json_hexn("b", r2, 2); json_obj_end();
            json_str("why", "pixel mask register 3C6 is 8-bit read/write (IBM VGA TR)");
            json_obj("ctx"); rtg_ctx_card(); json_obj_end();
            amt_case_close();
        }
    }
}

void vga_std_tests(void)
{
    UBYTE m0, m1;
    idx_test("vga_sr_index", 0x3c4, 0x07, "sequencer index 3C4 does not read back (bits 2:0, IBM VGA TR)");
    idx_test("vga_gr_index", 0x3ce, 0x0f, "graphics controller index 3CE does not read back (bits 3:0)");
    idx_test("vga_cr_index", 0x3d4, 0x1f, "CRTC index 3D4 does not read back (bits 4:0)");
    rtg_regfile("vga_seq", 0x3c4, vga_sr, 2, "sequencer register (SR2 map mask 3:0, SR3 character map 5:0) does not read back");
    rtg_regfile("vga_gr", 0x3ce, vga_gr, 6, "graphics controller register (GR0-3,7 low bits, GR8 bit mask) does not read back");
    {
        UBYTE c11 = CR_R(0x11);
        CR_W(0x11, c11 & 0x7f);
        rtg_regfile("vga_crtc", 0x3d4, vga_cr, 8, "CRTC register (cursor start/end, start address, cursor location, offset, underline) does not read back");
        CR_W(0x11, c11);
    }
    /* CR11 bit 7 protects CR0-CR7 (IBM VGA TR) */
    {
        UBYTE c11 = CR_R(0x11), c1 = CR_R(0x01), r;
        CR_W(0x11, c11 | 0x80);
        CR_W(0x01, (UBYTE)(c1 ^ 0x5a));
        r = CR_R(0x01);
        CR_W(0x11, c11 & 0x7f);
        CR_W(0x01, c1);
        CR_W(0x11, c11);
        rtg_chk("vga_crtc_protect", "cr01", c1, r, "CR01 changed although CR11 bit 7 (protect CR0-7) was set (IBM VGA TR)");
    }
    attr_test();
    /* misc output: written at 3C2, read at 3CC */
    m0 = vi(0x3cc);
    vo(0x3c2, (UBYTE)((m0 & 0x01) | 0xe2));
    m1 = vi(0x3cc);
    vo(0x3c2, m0);
    rtg_chk("vga_misc", "misc", (UBYTE)((m0 & 0x01) | 0xe2), m1, "miscellaneous output register written at 3C2 does not read back at 3CC (IBM VGA TR)");
    dac_test();
}

/* 640x480, 60 Hz, 25.175 MHz (IBM VGA mode 12h timings), 8 bits per pixel
 * graphics; chip-specific packed-pixel enables are set by the caller. */
static const UBYTE m480_cr[0x19] = {
    0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0x0b, 0x3e, 0x00, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xea, 0x8c, 0xdf, 0x50, 0x00, 0xe7, 0x04, 0xe3, 0xff
};
static const UBYTE m480_sr[5] = { 0x03, 0x01, 0x0f, 0x00, 0x0e };
static const UBYTE m480_gr[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0f, 0xff };

void vga_set_640x480(void)
{
    int i;
    vo(0x3c2, 0xe3);
    SR_W(0, 0x01);                 /* synchronous reset while changing timing */
    for (i = 1; i < 5; i++) SR_W(i, m480_sr[i]);
    SR_W(0, 0x03);
    CR_W(0x11, CR_R(0x11) & 0x7f);
    for (i = 0; i < 0x19; i++) CR_W(i, m480_cr[i]);
    for (i = 0; i < 9; i++) GR_W(i, m480_gr[i]);
    for (i = 0; i < 16; i++) ar_w(i, i);
    ar_w(0x10, 0x41); ar_w(0x11, 0x00); ar_w(0x12, 0x0f); ar_w(0x13, 0x00); ar_w(0x14, 0x00);
    (void)vi(0x3da);
    vo(0x3c0, 0x20);               /* PAS: video on */
    vo(DAC_M, 0xff);
}

void vga_palette_load(void)
{
    int i;
    vo(DAC_WI, 0);
    for (i = 0; i < 256; i++) {
        vo(DAC_D, (UBYTE)((i >> 2) & 0x3f));          /* R ramp */
        vo(DAC_D, (UBYTE)((i * 7) & 0x3f));           /* G */
        vo(DAC_D, (UBYTE)(((255 - i) >> 2) & 0x3f));  /* B */
    }
}

int vga_check_640x480(const char *suffix)
{
    int i, bad = -1;
    UBYTE e = 0, g = 0;
    const char *what = "";
    g = vi(0x3cc);
    if (g != 0xe3) { bad = 0; e = 0xe3; what = "misc"; }
    for (i = 1; i < 5 && bad < 0; i++) {
        g = SR_R(i);
        if (g != m480_sr[i] && !(i == 4 && (g & 0x0e) == 0x0e)) { bad = i; e = m480_sr[i]; what = "sr"; }
    }
    for (i = 0; i < 0x19 && bad < 0; i++) {
        g = CR_R(i);
        if (g != m480_cr[i]) { bad = i; e = m480_cr[i]; what = "cr"; }
    }
    for (i = 0; i < 9 && bad < 0; i++) {
        UBYTE m = (i == 5) ? 0x7b : (i == 4 ? 0x03 : 0xff);
        g = GR_R(i);
        if ((g ^ m480_gr[i]) & m) { bad = i; e = m480_gr[i]; what = "gr"; }
    }
    if (bad < 0) { rtg_pass(suffix); return 1; }
    amt_case_open(rtg_cid(suffix), AMT_ST_FAIL);
    json_obj("expect"); json_hexn("value", e, 2); json_obj_end();
    json_obj("got"); json_hexn("value", g, 2); json_obj_end();
    json_str("why", "a 640x480 mode register does not read back after the mode set (standard VGA registers)");
    json_obj("in"); json_str("reg", what); json_hexn("index", (ULONG)bad, 2); json_obj_end();
    json_obj("ctx"); rtg_ctx_card(); json_obj_end();
    amt_case_close();
    return 0;
}

/* Input status 1 (3DA) bit 3 = vertical retrace: with the CRTC running it
 * must be seen both set and clear (IBM VGA TR). Polls up to ~3 Amiga frames. */
void vga_retrace_test(void)
{
    ULONG f0 = amt_frames_now();
    int seen0 = 0, seen1 = 0, de0 = 0, de1 = 0;
    while (amt_frames_now() - f0 < 4 && !(seen0 && seen1 && de0 && de1)) {
        UBYTE s = vi(0x3da);
        if (s & 8) seen1 = 1; else seen0 = 1;
        if (s & 1) de1 = 1; else de0 = 1;
    }
    if (seen0 && seen1)
        rtg_pass("vga_vretrace");
    else {
        amt_case_open(rtg_cid("vga_vretrace"), AMT_ST_FAIL);
        json_obj("expect"); json_str("vr", "toggles"); json_obj_end();
        json_obj("got"); json_int("seen_clear", seen0); json_int("seen_set", seen1); json_obj_end();
        json_str("why", "input status 1 (3DA) bit 3 (vertical retrace) did not toggle within 4 Amiga frames with a 60 Hz 640x480 CRTC programmed");
        json_obj("ctx"); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
    if (de0 && de1)
        rtg_pass("vga_display_enable");
    else {
        amt_case_open(rtg_cid("vga_display_enable"), AMT_ST_FAIL);
        json_obj("expect"); json_str("de", "toggles"); json_obj_end();
        json_obj("got"); json_int("seen_clear", de0); json_int("seen_set", de1); json_obj_end();
        json_str("why", "input status 1 (3DA) bit 0 (display disabled / blanking) did not toggle within 4 Amiga frames");
        json_obj("ctx"); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
}

/* ------------------------------------------------------------ vertical interrupt */
static volatile ULONG vint_f1, vint_count, vint_foreign, vint_stuck, vint_f_is0, vint_f_req, vint_f_cnt;
static UBYTE vint_level;
static struct cia_saved vint_cia;
static UBYTE vint_c11, vint_crx;
static int vint_stage, vint_cleaning;
static UWORD vint_intena;
static void (*vint_enable)(int);
static void (*vint_old_hook)(ULONG);

static void vint_restore(void)
{
    int stage = vint_stage;
    UWORD bits = vint_level == 2 ? 0x0008 : 0x2000;
    if (!stage) return;
    if (vint_cleaning) amt_abort_fatal(AMT_ABORT_UNEXPECTED);
    vint_cleaning = 1;
    CR_W(0x11, (vint_c11 | 0x20) & ~0x10);
    if (stage & 4) {
        CREG(R_INTENA) = bits;
        if (vint_enable) vint_enable(0);
        CREG(R_INTREQ) = bits;
        amt_irq_hook[vint_level] = vint_old_hook;
    }
    if (stage & 2) cia_restore(&vint_cia);
    CR_W(0x11, vint_c11);
    vo(0x3d4, vint_crx);
    if ((stage & 4) && vint_intena) CREG(R_INTENA) = 0x8000 | vint_intena;
    vint_stage = vint_cleaning = 0;
}

/* level 2/6 hook: count only interrupts the card reports pending (input
 * status 0 bit 7). Anything else on that level (a CIA left enabled by the
 * OS) is not ours: the level is masked again so it cannot storm, and the
 * case becomes untestable. CIA registers are never touched. */
static void vint_hook(ULONG pending)
{
    UWORD bits = vint_level == 2 ? 0x0008 : 0x2000;
    if (vi(0x3c2) & 0x80) {
        /* acknowledge at the chip: CR11 bit 4 = 0 clears, then 1 re-arms */
        UBYTE x = vi(0x3d4), c11;
        vo(0x3d4, 0x11);
        c11 = vi(0x3d5);
        vo(0x3d5, c11 & ~0x10);
        vo(0x3d5, c11 | 0x10);
        vo(0x3d4, x);
        if (vint_count == 0) vint_f1 = amt_frames_now();
        vint_count++;
        if (vint_count >= 3) {
            /* three periodic interrupts span at least two card frames (33 ms
             * at 60 Hz), so more than one Amiga frame; all three within
             * the same Amiga frame means the acknowledge did not clear it */
            if (amt_frames_now() == vint_f1) vint_stuck = 1;
            CREG(R_INTENA) = bits;
        }
    } else {
        if (!vint_foreign++) { vint_f_is0 = vi(0x3c2); vint_f_req = CREG(R_INTREQR); vint_f_cnt = vint_count; }
        CREG(R_INTENA) = bits;
    }
    CREG(R_INTREQ) = (UWORD)(pending & bits);
}

static void vint_run(void)
{
    void (*board_enable)(int) = vint_enable;
    /* CR11 bit 5 = 0 enables the vertical interrupt, bit 4 = 0 clears it;
     * input status 0 (3C2 read) bit 7 = interrupt pending (IBM VGA TR) */
    UBYTE c11 = CR_R(0x11), s0a, s0b = 0, s0c;
    ULONG f0;
    int got = 0;
    CR_W(0x11, (c11 & ~0x30));         /* enabled, cleared */
    CR_W(0x11, (c11 & ~0x20) | 0x10);  /* enabled, armed */
    s0a = vi(0x3c2);
    f0 = amt_frames_now();
    while (amt_frames_now() - f0 < 4) {
        s0b = vi(0x3c2);
        if (s0b & 0x80) { got = 1; break; }
    }
    CR_W(0x11, (c11 & ~0x30));         /* clear */
    s0c = vi(0x3c2);
    CR_W(0x11, c11 | 0x20);            /* disable */
    CR_W(0x11, (c11 | 0x30));
    CR_W(0x11, c11);
    if (got && !(s0c & 0x80))
        rtg_pass("vga_vint_status");
    else {
        amt_case_open(rtg_cid("vga_vint_status"), AMT_ST_FAIL);
        json_obj("expect"); json_int("pending_seen", 1); json_int("pending_after_clear", 0); json_obj_end();
        json_obj("got"); json_int("pending_seen", got); json_int("pending_after_clear", (s0c >> 7) & 1); json_obj_end();
        json_str("why", "with CR11 bit 5 = 0 (vertical interrupt enabled) input status 0 (3C2) bit 7 must become 1 within a frame of the card, and CR11 bit 4 = 0 must clear it (IBM VGA TR)");
        json_obj("ctx"); json_hexn("is0_start", s0a, 2); json_hexn("is0_last", s0b, 2); json_hexn("cr11", c11, 2); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }

    /* Amiga level */
    if (!rtg_cur->irq || !board_enable) {
        return;
    }
    vint_level = rtg_cur->irq;
    /* the CIA on the same level (A INT2, B INT6) may have sources the OS
     * enabled: its ICR is masked for the test and restored afterwards
     * (cia_hw.h: mask from ciaX.resource, pending events re-posted) */
    cia_save(&vint_cia, vint_level == 2 ? CIAA_BASE : CIAB_BASE, 0);
    vint_stage |= 2;
    vint_count = 0;
    vint_foreign = 0;
    vint_stuck = 0;
    {
        UWORD bits = vint_level == 2 ? 0x0008 : 0x2000;
        vint_intena = CREG(R_INTENAR) & bits;
        vint_old_hook = amt_irq_hook[vint_level];
        vint_stage |= 4;
        CREG(R_INTREQ) = bits;
        amt_irq_hook[vint_level] = vint_hook;
        CREG(R_INTENA) = (UWORD)(0x8000 | bits);
        board_enable(1);
        CR_W(0x11, (c11 & ~0x30));
        CR_W(0x11, (c11 & ~0x20) | 0x10);
        f0 = amt_frames_now();
        while (amt_frames_now() - f0 < 10 && vint_count < 3)
            ;
    }
    vint_restore();
    if (vint_count >= 3 && !vint_stuck)
        rtg_pass("vint_amiga");
    else if (vint_foreign) {
        amt_case_open(rtg_cid("vint_amiga"), AMT_ST_UNTESTABLE);
        json_str("why", "another interrupt source was active at the board's level (with that level's CIA masked); the level was masked again");
        json_obj("ctx"); json_int("level", vint_level); json_int("card_interrupts", (long)vint_f_cnt);
        json_hexn("is0", vint_f_is0, 2); json_hexn("intreqr", vint_f_req, 4); json_obj_end();
        amt_case_close();
    }
    else if (rtg_cur->irq_inferred && vint_count == 0) {
        /* review M10: the level is inferred from the chip (a GD5428
         * Picasso II is taken for a II+), not known from the board id; a
         * Picasso II with a GD5428 and no interrupt line is legal */
        amt_case_open(rtg_cid("vint_amiga"), AMT_ST_UNTESTABLE);
        json_str("why", "no interrupt arrived, and this board's interrupt support is only inferred from its chip "
                        "(Picasso II with a GD5428 taken for a II+): it may have no interrupt line");
        json_obj("ctx"); json_int("level", vint_level); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
    else {
        amt_case_open(rtg_cid("vint_amiga"), AMT_ST_FAIL);
        json_obj("expect"); json_str("interrupts", ">=3 in 10 frames"); json_obj_end();
        json_obj("got"); json_int("interrupts", (long)vint_count); json_int("storm", (long)vint_stuck); json_obj_end();
        json_str("why", "the card's vertical interrupt (CR11 bit 5 = 0, board interrupt enabled) did not reach the Amiga at the board's interrupt level as one interrupt per card frame (storm = 1: three within one Amiga frame, the CR11 bit 4 acknowledge did not remove the request)");
        json_obj("ctx"); json_int("level", vint_level); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
}

void vga_vint_test(void (*board_enable)(int on))
{
    int code, cleanup;
    vint_enable = board_enable;
    vint_level = rtg_cur->irq;
    vint_crx = vi(0x3d4);
    vint_c11 = CR_R(0x11);
    vint_stage = 1;
    vint_cleaning = 0;
    code = amt_try(vint_run);
    cleanup = amt_try(vint_restore);
    if (cleanup) amt_abort_fatal(cleanup);
    if (code) amt_abort(code);
}

/* ------------------------------------------------------------ VRAM */
static void vram_fail(const char *suffix, const char *why, ULONG off, int width,
                      ULONG expect, ULONG got, const char *win)
{
    amt_case_open(rtg_cid(suffix), AMT_ST_FAIL);
    json_obj("expect"); json_hex("value", expect); json_obj_end();
    json_obj("got"); json_hex("value", got); json_obj_end();
    json_str("why", why);
    json_obj("in"); json_hex("offset", off); json_int("width", width); json_str("window", win); json_obj_end();
    json_obj("ctx"); rtg_ctx_card(); json_obj_end();
    amt_case_close();
}

/* write in one width, read in all three; natural window */
static void vram_bwl(void)
{
    static const ULONG offs[4] = { 0x0, 0x1234, 0x8000, 0xfffc };
    volatile UBYTE *p = rtg_cur->win[0].p;
    int i;
    for (i = 0; i < 4; i++) {
        ULONG o = offs[i];
        volatile UBYTE *b = p + o;
        volatile UWORD *w = (volatile UWORD *)(p + o);
        volatile ULONG *l = (volatile ULONG *)(p + o);
        ULONG save = *l, v;
        *l = 0x11223344UL;
        v = ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) | ((ULONG)b[2] << 8) | b[3];
        if (v != 0x11223344UL) { *l = save; vram_fail("vram_bwl", "long write, byte reads: bytes not in big-endian order at the natural window", o, 1, 0x11223344UL, v, "natural"); return; }
        v = ((ULONG)w[0] << 16) | w[1];
        if (v != 0x11223344UL) { *l = save; vram_fail("vram_bwl", "long write, word reads differ", o, 2, 0x11223344UL, v, "natural"); return; }
        b[0] = 0xa1; b[1] = 0xa2; b[2] = 0xa3; b[3] = 0xa4;
        v = *l;
        if (v != 0xa1a2a3a4UL) { *l = save; vram_fail("vram_bwl", "byte writes, long read differs", o, 4, 0xa1a2a3a4UL, v, "natural"); return; }
        w[0] = 0xb1b2; w[1] = 0xb3b4;
        v = *l;
        if (v != 0xb1b2b3b4UL) { *l = save; vram_fail("vram_bwl", "word writes, long read differs", o, 4, 0xb1b2b3b4UL, v, "natural"); return; }
        v = b[1] | ((ULONG)b[2] << 8);
        if (v != 0xb3b2UL) { *l = save; vram_fail("vram_bwl", "word writes, odd/even byte reads differ", o + 1, 1, 0xb3b2UL, v, "natural"); return; }
        b[3] = 0x5c;
        v = *l;
        *l = save;
        if (v != 0xb1b2b35cUL) { vram_fail("vram_bwl", "a byte write changed a neighbouring byte", o + 3, 1, 0xb1b2b35cUL, v, "natural"); return; }
    }
    rtg_pass("vram_bwl");
}

/* address lines inside the first 64 KB: unique longs at 2^k */
static void vram_addr_lines(void)
{
    volatile UBYTE *p = rtg_cur->win[0].p;
    ULONG save[15], o;
    int k;
    for (k = 2, o = 4; k < 16; k++, o <<= 1) save[k - 2] = *(volatile ULONG *)(p + o);
    save[14] = *(volatile ULONG *)p;
    *(volatile ULONG *)p = 0xc0ffee00UL;
    for (k = 2, o = 4; k < 16; k++, o <<= 1) *(volatile ULONG *)(p + o) = 0xa5000000UL | ((ULONG)k << 8) | (UBYTE)~k;
    for (k = 2, o = 4; k < 16; k++, o <<= 1) {
        ULONG e = 0xa5000000UL | ((ULONG)k << 8) | (UBYTE)~k, g = *(volatile ULONG *)(p + o);
        if (g != e) {
            vram_fail("vram_addr_lines", "unique longs written at offsets 2^k (k=2..15) do not read back: an address line aliases", o, 4, e, g, "natural");
            goto out;
        }
    }
    if (*(volatile ULONG *)p != 0xc0ffee00UL) {
        vram_fail("vram_addr_lines", "offset 0 was overwritten by a write at a power-of-two offset", 0, 4, 0xc0ffee00UL, *(volatile ULONG *)p, "natural");
        goto out;
    }
    rtg_pass("vram_addr_lines");
out:
    for (k = 2, o = 4; k < 16; k++, o <<= 1) *(volatile ULONG *)(p + o) = save[k - 2];
    *(volatile ULONG *)p = save[14];
}

/* one long per 64 KB at the same in-block offset: VRAM size and mirroring */
#define WALK_MAX 256
static ULONG walk_save[WALK_MAX];
static ULONG walk_pending;
static volatile ULONG *alias_pending;
static ULONG alias_saved[2];
static int alias_longs;

void rtg_vram_restore(void)
{
    ULONG i;
    if (alias_pending) {
        for (i = 0; i < (ULONG)alias_longs; i++) alias_pending[i] = alias_saved[i];
        alias_pending = 0;
    }
    if (walk_pending) {
        volatile UBYTE *p = rtg_cur->win[0].p;
        for (i = walk_pending; i-- > 0;)
            *(volatile ULONG *)(p + (i << 16) + 0x100) = walk_save[i];
        walk_pending = 0;
    }
}

static void vram_walk(void)
{
    struct rtg_card *c = rtg_cur;
    volatile UBYTE *p = c->win[0].p;
    ULONG lim = c->win[0].size, n, i, good = 0, sz;
    ULONG first_bad = 0, fexp = 0, fgot = 0;
    if (lim > c->vram_max) lim = c->vram_max;
    n = lim >> 16;
    if (n > WALK_MAX) n = WALK_MAX;
    for (i = 0; i < n; i++) walk_save[i] = *(volatile ULONG *)(p + (i << 16) + 0x100);
    walk_pending = n;
    /* highest block first: on a mirror the lower block's write wins, so
     * the first block that does not read back its own value is the size */
    for (i = n; i-- > 0;) *(volatile ULONG *)(p + (i << 16) + 0x100) = 0x5a000000UL | (i << 8) | (UBYTE)~i;
    for (i = 0; i < n; i++) {
        ULONG e = 0x5a000000UL | (i << 8) | (UBYTE)~i, g = *(volatile ULONG *)(p + (i << 16) + 0x100);
        if (g != e) { first_bad = i; fexp = e; fgot = g; break; }
        good++;
    }
    for (i = n; i-- > 0;) *(volatile ULONG *)(p + (i << 16) + 0x100) = walk_save[i];
    walk_pending = 0;
    sz = good << 16;
    /* Only the known 3.625 MB CV64/3D Z2 aperture can establish window
     * uniqueness without observing the card's entire minimum VRAM. */
    if ((sz >= c->vram_min && (sz & (sz - 1)) == 0) ||
        (c->kind == K_CV643D_Z2 && lim == 0x3a0000UL && good == n)) {
        amt_case_open(rtg_cid("vram_walk"), AMT_ST_PASS);
        json_obj("ctx"); json_int("vram_kb", (long)(sz >> 10)); json_int("window_kb", (long)(lim >> 10)); json_obj_end();
        amt_case_close();
        return;
    }
    amt_case_open(rtg_cid("vram_walk"), AMT_ST_FAIL);
    json_obj("expect"); json_str("vram", "power of two >= card minimum, unique per 64 KB"); json_int("min_kb", (long)(c->vram_min >> 10)); json_obj_end();
    json_obj("got"); json_int("good_kb", (long)(sz >> 10)); json_hex("value", fgot); json_obj_end();
    json_str("why", "a sample per 64 KB over the VRAM window does not read back uniquely from offset 0 up to a power-of-two size of at least the card's minimum VRAM");
    json_obj("in"); json_hex("offset", (first_bad << 16) + 0x100); json_hex("written", fexp); json_int("window_kb", (long)(lim >> 10)); json_obj_end();
    json_obj("ctx"); rtg_ctx_card(); json_obj_end();
    amt_case_close();
}

/* a byte-swapped window against the natural one (lane model: CPU byte at
 * offset o of the window is VRAM byte o ^ mask). Word and long accesses
 * must follow it exactly. For single byte accesses through a swapped
 * window both "lane swapped" and "unswapped" are accepted (boards differ:
 * some swap by data lanes, some only for 16/32-bit cycles). */
static ULONG lane(volatile UBYTE *nat, ULONG o, UBYTE m, int width)
{
    ULONG v = 0;
    int i;
    for (i = 0; i < width; i++) v = (v << 8) | nat[(o + i) ^ m];
    return v;
}

static void vram_swap(int wi)
{
    struct rtg_win *w = &rtg_cur->win[wi];
    volatile UBYTE *nat = rtg_cur->win[0].p, *sw = w->p;
    char suf[40], *d = suf;
    const char *s = "vram_swap_";
    ULONG o = 0x2340, save0, save1, v, e;
    int bytemode = -1;
    while (*s) *d++ = *s++;
    s = w->name;
    while (*s && d < suf + 30) *d++ = *s++;
    *d = 0;
    save0 = *(volatile ULONG *)(nat + o);
    save1 = *(volatile ULONG *)(nat + o + 4);
    alias_saved[0] = *(volatile ULONG *)(sw + o);
    alias_saved[1] = *(volatile ULONG *)(sw + o + 4);
    alias_longs = 2;
    alias_pending = (volatile ULONG *)(sw + o);
    /* natural long write, swapped reads */
    *(volatile ULONG *)(nat + o) = 0x01234567UL;
    *(volatile ULONG *)(nat + o + 4) = 0x89abcdefUL;
    e = lane(nat, o, w->mask, 4);
    v = *(volatile ULONG *)(sw + o);
    if (v != e) { vram_fail(suf, "long read through the swapped window does not give the byte-lane swapped view of the natural window", o, 4, e, v, w->name); goto out; }
    e = lane(nat, o + 6, w->mask, 2);
    v = *(volatile UWORD *)(sw + o + 6);
    if (v != e) { vram_fail(suf, "word read through the swapped window does not give the byte-lane swapped view", o + 6, 2, e, v, w->name); goto out; }
    /* swapped writes, natural reads */
    *(volatile ULONG *)(sw + o) = 0xdeadbeefUL;
    v = lane(nat, o, w->mask, 4);
    if (v != 0xdeadbeefUL) { vram_fail(suf, "long write through the swapped window, natural bytes are not lane swapped", o, 4, 0xdeadbeefUL, v, w->name); goto out; }
    *(volatile UWORD *)(sw + o + 4) = 0x1357;
    v = lane(nat, o + 4, w->mask, 2);
    if (v != 0x1357) { vram_fail(suf, "word write through the swapped window, natural bytes are not lane swapped", o + 4, 2, 0x1357, v, w->name); goto out; }
    /* bytes */
    *(volatile ULONG *)(nat + o) = 0;
    sw[o + 1] = 0xa7;
    if (nat[(o + 1) ^ w->mask] == 0xa7 && nat[o + 1] == (((o + 1) ^ w->mask) == o + 1 ? 0xa7 : 0)) bytemode = 1;
    else if (nat[o + 1] == 0xa7 && nat[(o + 1) ^ w->mask] == 0) bytemode = 0;
    if (bytemode < 0) {
        vram_fail(suf, "byte write through the swapped window landed neither at the lane-swapped nor at the same natural byte", o + 1, 1, 0xa7, lane(nat, o, 0, 4), w->name);
        goto out;
    }
    amt_case_open(rtg_cid(suf), AMT_ST_PASS);
    json_obj("ctx"); json_int("mask", w->mask); json_int("bytes_lane_swapped", bytemode); json_obj_end();
    amt_case_close();
out:
    rtg_vram_restore();       /* also restore a window that failed to alias */
    *(volatile ULONG *)(nat + o) = save0;
    *(volatile ULONG *)(nat + o + 4) = save1;
}

void rtg_vram_tests(void)
{
    int i;
    if (!rtg_cur->nwin)
        return;
    vram_bwl();
    vram_addr_lines();
    vram_walk();
    for (i = 1; i < rtg_cur->nwin; i++) {
        if (rtg_cur->win[i].mask)
            vram_swap(i);
        else {
            /* an alias of the natural window: same bytes */
            volatile UBYTE *a = rtg_cur->win[i].p, *n = rtg_cur->win[0].p;
            ULONG s = *(volatile ULONG *)(n + 0x3450), g;
            char suf[40], *d = suf;
            const char *t = "vram_alias_";
            while (*t) *d++ = *t++;
            t = rtg_cur->win[i].name;
            while (*t && d < suf + 30) *d++ = *t++;
            *d = 0;
            alias_saved[0] = *(volatile ULONG *)(a + 0x3450);
            alias_longs = 1;
            alias_pending = (volatile ULONG *)(a + 0x3450);
            *(volatile ULONG *)(n + 0x3450) = 0x600dcafeUL;
            g = *(volatile ULONG *)(a + 0x3450);
            *(volatile ULONG *)(a + 0x3450) = 0x0badf00dUL;
            {
                ULONG g2 = *(volatile ULONG *)(n + 0x3450);
                rtg_vram_restore();
                *(volatile ULONG *)(n + 0x3450) = s;
                if (g == 0x600dcafeUL && g2 == 0x0badf00dUL)
                    rtg_pass(suf);
                else if (g != 0x600dcafeUL)
                    vram_fail(suf, "natural-to-alias read differs", 0x3450, 4, 0x600dcafeUL, g, rtg_cur->win[i].name);
                else
                    vram_fail(suf, "alias-to-natural read differs", 0x3450, 4, 0x0badf00dUL, g2, rtg_cur->win[i].name);
            }
        }
    }
}

void rtg_seed_rect(ULONG a, int w, int h, ULONG pitch, UBYTE *copy)
{
    UBYTE *seed = rtg_buf + 12288;
    volatile UBYTE *p = rtg_cur->win[0].p;
    int x, y, i = 0;
    if (w <= 0 || h <= 0 || w > 4096 / h || pitch < (ULONG)w ||
        a > RTG_WORK || (ULONG)(h - 1) > (RTG_WORK - a) / pitch ||
        (ULONG)w > RTG_WORK - a - (ULONG)(h - 1) * pitch)
        amt_abort(AMT_ABORT_UNEXPECTED);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++, i++) {
            seed[i] = (UBYTE)(amt_rand() >> 13);
            if (copy) copy[i] = seed[i];
            p[a + (ULONG)y * pitch + x] = seed[i];
        }
    i = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++, i++) {
            UBYTE g = p[a + (ULONG)y * pitch + x];
            if (g != seed[i]) {
                vram_fail("engine_seed", "CPU-generated input did not read back before submitting an engine command",
                          a + (ULONG)y * pitch + x, 1, seed[i], g, "natural");
                amt_abort(AMT_ABORT_UNEXPECTED);
            }
        }
}

/* pattern in an 8-bit mode at `pitch` bytes per line, `lines` lines,
 * through the natural window, read back by CRC */
void rtg_draw_test(const char *suffix, ULONG pitch, int lines)
{
    volatile UBYTE *p = rtg_cur->win[0].p;
    ULONG crc_e = 0, crc_g = 0, x;
    int y, bad = -1;
    UBYTE *row = rtg_buf;
    for (y = 0; y < lines; y++) {
        volatile ULONG *d = (volatile ULONG *)(p + y * pitch);
        for (x = 0; x < pitch; x++)
            row[x] = (UBYTE)((x ^ (y << 1)) + (x >> 4));
        for (x = 0; x < pitch / 4; x++)
            d[x] = ((ULONG *)row)[x];
        crc_e = crc32_update(crc_e, row, pitch);
    }
    for (y = 0; y < lines; y++) {
        volatile ULONG *s = (volatile ULONG *)(p + y * pitch);
        for (x = 0; x < pitch / 4; x++)
            ((ULONG *)row)[x] = s[x];
        crc_g = crc32_update(crc_g, row, pitch);
        if (bad < 0)
            for (x = 0; x < pitch; x++)
                if (row[x] != (UBYTE)((x ^ (y << 1)) + (x >> 4))) { bad = (int)(y * pitch + x); break; }
    }
    if (bad < 0) { rtg_pass(suffix); return; }
    amt_case_open(rtg_cid(suffix), AMT_ST_FAIL);
    json_obj("expect"); json_hex("crc", crc_e); json_obj_end();
    json_obj("got"); json_hex("crc", crc_g); json_obj_end();
    json_str("why", "a pattern drawn by the CPU into VRAM in the 8-bit mode does not read back");
    json_obj("in"); json_hex("first_bad", (ULONG)bad); json_int("pitch", (long)pitch); json_int("lines", lines); json_obj_end();
    json_obj("ctx"); rtg_ctx_card(); json_obj_end();
    amt_case_close();
}

static int monitor_pending;

void rtg_monitor_restore(void)
{
    if (monitor_pending) {
        rtg_monswitch(0);
        monitor_pending = 0;
    }
}

int rtg_screenshot(const char *suffix, const char *tag)
{
    int ok;
    monitor_pending = 1;
    rtg_monswitch(1);
    ok = amt_request("screenshot", tag, "{\"frames\":3}", amt_secs(10));
    rtg_monitor_restore();
    if (ok)
        rtg_pass(suffix);
    else
        rtg_untestable(suffix, "host did not act on request screenshot");
    return ok;
}

/* ------------------------------------------------------------ board level */
/* Monitor switch / board interrupt enable. No public documentation exists
 * for these board registers; the addresses follow the Picasso96 drivers as
 * modelled by WinUAE/Amiberry. They only make the RTG output visible for
 * the screenshot and let the vertical interrupt reach the Amiga. */
UBYTE rtg_cv64_ctl_shadow;

void rtg_cv64_control(UBYTE value)
{
    rtg_cv64_ctl_shadow = value;
    rtg_cur->mem[0x40001] = value;
}

void rtg_monswitch(int on)
{
    struct rtg_card *c = rtg_cur;
    switch (c->kind) {
    case K_PICASSO2:
        c->regs[on ? 0xa000 : 0xb000] = 0;
        break;
    case K_PICCOLO:
        c->regs[0x8000] = on ? 0x20 : 0x00;
        break;
    case K_CV64:
        rtg_cv64_control((UBYTE)((rtg_cv64_ctl_shadow & ~0x10) | (on ? 0x10 : 0)));
        break;
    case K_CV643D_Z3:
        *(volatile UWORD *)(c->mem + 0x8000000) = on ? 0 : 1;   /* bit 0 = 0: card output */
        break;
    case K_CV643D_Z2:
        *(volatile UWORD *)(c->mem + 0x3a0000) = on ? 0 : 1;
        break;
    default:
        break;
    }
}

void rtg_board_irq(int on)
{
    struct rtg_card *c = rtg_cur;
    switch (c->kind) {
    case K_PICASSO2:
        c->regs[on ? 0x9001 : 0x9000] = 0;
        break;
    case K_PICCOLO:
        c->regs[0x8000] = on ? 0x40 : 0x00;
        break;
    case K_CV64:
        /* bit 3 interrupt enable, bit 7 level (0 = INT6, 1 = INT2) */
        rtg_cv64_control((UBYTE)((rtg_cv64_ctl_shadow & ~0x88) | (on ? 0x08 : 0)));
        break;
    case K_CV643D_Z3:
        *(volatile UWORD *)(c->mem + 0x8000000 + 12) = on ? 2 : 0;  /* bit 1 interrupt enable */
        break;
    case K_CV643D_Z2:
        *(volatile UWORD *)(c->mem + 0x3a0000 + 12) = on ? 2 : 0;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ boards */
struct cdef {
    UWORD man;
    UBYTE pmem, preg;     /* preg 0xff: single board */
    UBYTE kind, chip, bus; /* bus 0: either */
    UBYTE irq;
    const char *id, *name;
    UBYTE ids[2];
    ULONG vmin, vmax;
};

static const struct cdef cdefs[] = {
    { 2167, 11, 12, K_PICASSO2, CH_CIRRUS, 2, 0, "picasso2", "Picasso II/II+ (GD5426/5428)", {0x90, 0x98}, 0x100000, 0x200000 },
    { 2167, 21, 23, K_PICASSO4_Z2, CH_CIRRUS, 2, 2, "picasso4", "Picasso IV Z2 (GD5446)", {0xb8, 0xb8}, 0x200000, 0x200000 },
    { 2167, 24, 23, K_PICASSO4_Z2, CH_CIRRUS, 2, 2, "picasso4", "Picasso IV Z2 (GD5446)", {0xb8, 0xb8}, 0x200000, 0x200000 },
    { 2167, 24, 0xff, K_PICASSO4_Z3, CH_CIRRUS, 3, 2, "picasso4", "Picasso IV Z3 (GD5446)", {0xb8, 0xb8}, 0x200000, 0x400000 },
    { 2195, 5, 6, K_PICCOLO, CH_CIRRUS, 0, 6, "piccolo", "Piccolo (GD5426)", {0x90, 0x90}, 0x100000, 0x200000 },
    { 2195, 10, 11, K_PICCOLO, CH_CIRRUS, 0, 6, "sd64", "Piccolo SD64 (GD5434)", {0xa8, 0xa4}, 0x200000, 0x400000 },
    { 2193, 1, 2, K_PICCOLO, CH_CIRRUS, 0, 6, "spectrum", "Spectrum 28/24 (GD5428)", {0x98, 0x98}, 0x100000, 0x200000 },
    { 2092, 34, 33, K_GRAFFITY_Z2, CH_CIRRUS, 2, 0, "graffity", "Graffity Z2 (GD5428)", {0x98, 0x98}, 0x100000, 0x200000 },
    { 2092, 33, 0xff, K_OTHER, CH_CIRRUS, 3, 0, "graffity", "Graffity Z3 (GD5428)", {0x98, 0x98}, 0x100000, 0x200000 },
    { 2026, 255, 254, K_PIXEL64, CH_CIRRUS, 2, 0, "pixel64", "Pixel64 (GD5434)", {0xa8, 0xa4}, 0x200000, 0x200000 },
    { 8512, 34, 0xff, K_CV64, CH_S3TRIO, 3, 6, "cv64", "CyberVision 64 (S3 Trio64)", {0, 0}, 0x200000, 0x400000 },
    { 8512, 67, 0xff, K_CV643D_Z3, CH_S3VIRGE, 3, 2, "cv643d", "CyberVision 64/3D (S3 ViRGE)", {0, 0}, 0x400000, 0x400000 },
    { 8512, 67, 0xff, K_CV643D_Z2, CH_S3VIRGE, 2, 2, "cv643d", "CyberVision 64/3D (S3 ViRGE)", {0, 0}, 0x400000, 0x3a0000 },
    { 2181, 0, 0, K_OMNIBUS, CH_ET4000, 2, 0, "omnibus", "oMniBus (ET4000)", {0, 0}, 0x100000, 0x100000 },
    { 2167, 1, 2, K_DOMINO, CH_ET4000, 2, 0, "domino", "Domino (ET4000)", {0, 0}, 0x100000, 0x100000 },
    { 2117, 3, 4, K_MERLIN, CH_ET4000, 0, 0, "merlin", "Merlin (ET4000W32)", {0, 0}, 0x200000, 0x200000 },
    { 1030, 0, 0xff, K_A2410, CH_TMS34010, 2, 0, "a2410", "A2410 (TMS34010)", {0, 0}, 0, 0 },
    { 28014, 3, 0xff, K_ZZ9000, CH_ZZ9000, 2, 0, "zz9000", "ZZ9000 Z2", {0, 0}, 0x100000, 0x3f0000 },
    { 28014, 4, 0xff, K_ZZ9000, CH_ZZ9000, 3, 0, "zz9000", "ZZ9000 Z3", {0, 0}, 0x100000, 0x400000 },
    { 2011, 96, 0xff, K_UAEGFX, CH_NONE, 0, 0, "uaegfx", "UAE RTG (uaegfx)", {0, 0}, 0, 0 },
    { 18260, 6, 0xff, K_OTHER, CH_NONE, 0, 0, "retina", "Retina Z2 (NCR 77C32BLT)", {0, 0}, 0, 0 },
    { 18260, 16, 0xff, K_OTHER, CH_NONE, 0, 0, "retina", "Retina Z3 (NCR 77C32BLT)", {0, 0}, 0, 0 },
    { 2117, 2, 1, K_OTHER, CH_NONE, 0, 0, "visiona", "Visiona (INMOS G300)", {0, 0}, 0, 0 },
    { 2145, 32, 0xff, K_OTHER, CH_NONE, 0, 0, "rainbow2", "Rainbow II", {0, 0}, 0, 0 },
    { 2145, 33, 0xff, K_OTHER, CH_NONE, 0, 0, "rainbow3", "Rainbow III (INMOS G300)", {0, 0}, 0, 0 },
    { 2118, 100, 0xff, K_OTHER, CH_NONE, 0, 0, "harlequin", "Harlequin", {0, 0}, 0, 0 },
};
#define NCDEF ((int)(sizeof cdefs / sizeof cdefs[0]))

static int board_bus(const struct amt_probe_board *b)
{
    return b->addr >= 0x01000000UL ? 3 : 2;
}

static void add_card(const struct cdef *d, const struct amt_probe_board *m, const struct amt_probe_board *r)
{
    struct rtg_card *c;
    if (ncards >= (int)(sizeof cards / sizeof cards[0]))
        return;
    c = &cards[ncards++];
    memset(c, 0, sizeof *c);
    c->id = d->id;
    c->name = d->name;
    c->kind = d->kind;
    c->chip = d->chip;
    c->irq = d->irq;
    c->ids[0] = d->ids[0];
    c->ids[1] = d->ids[1];
    c->nids = 2;
    c->man = d->man;
    c->prod_mem = d->pmem;
    c->prod_reg = d->preg;
    c->mem_board = m;
    c->reg_board = r;
    c->bus = (UWORD)board_bus(m);
    c->mem = (volatile UBYTE *)m->addr;
    c->memsize = m->size;
    c->vram_min = d->vmin;
    c->vram_max = d->vmax;
    if (r) {
        c->regs = (volatile UBYTE *)r->addr;
        c->regsize = r->size;
    }
}

/* known boards that amt_board_ok() rejected (shut up, no
 * address, outside the Zorro windows): reported as a skip, never touched */
static const struct cdef *rejected[8];
static ULONG rej_addr[8], rej_flags[8];
static int nrej;

static int known_board(const struct amt_probe_board *b)
{
    int k;
    for (k = 0; k < NCDEF; k++)
        if (cdefs[k].man == b->man && (cdefs[k].pmem == b->prod || cdefs[k].preg == b->prod))
            return k;
    return -1;
}

static void find_cards(void)
{
    int i, j, k;
    UBYTE used[AMT_PROBE_BOARDS_MAX];
    int nb = amt_probe_result.n_boards;
    if (nb > AMT_PROBE_BOARDS_MAX) nb = AMT_PROBE_BOARDS_MAX;
    memset(used, 0, sizeof used);
    nrej = 0;
    /* review H1: a board that fails the shared guard is never paired or
     * written; a recognised one is reported */
    for (i = 0; i < nb; i++) {
        const struct amt_probe_board *b = &amt_probe_result.boards[i];
        if (amt_board_ok(b))
            continue;
        used[i] = 1;
        k = known_board(b);
        if (k >= 0 && nrej < (int)(sizeof rejected / sizeof rejected[0])) {
            rejected[nrej] = &cdefs[k];
            rej_addr[nrej] = b->addr;
            rej_flags[nrej++] = b->flags;
        }
    }
    for (i = 0; i < nb; i++) {
        const struct amt_probe_board *b = &amt_probe_result.boards[i];
        if (used[i])
            continue;
        for (k = 0; k < NCDEF; k++) {
            const struct cdef *d = &cdefs[k];
            if (d->man != b->man || d->pmem != b->prod)
                continue;
            if (d->bus && d->bus != board_bus(b))
                continue;
            if (d->pmem == d->preg) {
                for (j = 0; j < nb; j++)
                    if (!used[j] && amt_probe_result.boards[j].man == d->man &&
                        amt_probe_result.boards[j].prod == d->pmem &&
                        amt_probe_result.boards[j].size > b->size)
                        break;
                if (j < nb) break;  /* register descriptor before memory */
            }
            if (d->preg == 0xff) {
                used[i] = 1;
                add_card(d, b, 0);
                break;
            }
            /* paired: the register board is a different board with preg;
             * when pmem == preg (oMniBus) the larger one is the memory */
            for (j = 0; j < nb; j++) {
                const struct amt_probe_board *r = &amt_probe_result.boards[j];
                if (j == i || used[j] || r->man != d->man || r->prod != d->preg)
                    continue;
                if (d->pmem == d->preg && r->size >= b->size)
                    continue;
                used[i] = used[j] = 1;
                add_card(d, b, r);
                break;
            }
            if (used[i])
                break;
            /* Keep recognised incomplete cards: run_card emits the
             * missing-register-board skip before deriving any pointers. */
            used[i] = 1;
            add_card(d, b, 0);
            break;
        }
    }
}

/* per-kind access layout (see the header comment for the sources) */
static int setup_card(struct rtg_card *c)
{
    volatile UBYTE *b = c->mem;
    switch (c->kind) {
    case K_PICASSO2:
    case K_PICCOLO:
    case K_GRAFFITY_Z2:
    case K_OMNIBUS:
    case K_DOMINO:
    case K_MERLIN:
        c->io = c->regs;
        c->dac_bt482 = c->kind == K_MERLIN;
        c->nwin = 1;
        c->win[0].p = b; c->win[0].size = c->memsize; c->win[0].name = "natural";
        break;
    case K_PIXEL64:
        c->io = c->regs;
        c->nwin = 2;
        c->win[0].p = b; c->win[0].size = 0x200000; c->win[0].name = "natural";
        if (c->memsize >= 0x400000) {
            c->win[1].p = b + 0x200000; c->win[1].size = 0x200000; c->win[1].mask = 1; c->win[1].name = "word";
        } else
            c->nwin = 1;
        break;
    case K_PICASSO4_Z2:
        c->io = c->regs + 0x10000;
        c->nwin = 1;
        c->win[0].p = b; c->win[0].size = c->memsize; c->win[0].name = "natural";
        break;
    case K_PICASSO4_Z3:
        /* The 64 MB variant uses a different layout. Until all apertures
         * are independently verified, reject it before any board access. */
        if (c->memsize != 0x2000000UL)
            return 0;
        c->io = b + 0x600000;
        c->regs = b;
        c->nwin = 3;
        c->win[0].p = b + 0x1000000; c->win[0].size = 0x400000; c->win[0].name = "natural";
        c->win[1].p = b + 0x1400000; c->win[1].size = 0x400000; c->win[1].mask = 1; c->win[1].name = "word";
        c->win[2].p = b + 0x800000; c->win[2].size = 0x400000; c->win[2].name = "vram2";
        break;
    case K_CV64:
        c->io = b + 0x2000000;
        c->nwin = 2;
        c->win[0].p = b + 0x1400000; c->win[0].size = 0x400000; c->win[0].name = "natural";
        c->win[1].p = b + 0x3400000; c->win[1].size = 0x400000; c->win[1].name = "upper";
        break;
    case K_CV643D_Z3:
        c->io = b + 0xc000000;
        c->ioxor = 3;
        c->nwin = 3;
        c->win[0].p = b + 0x4800000; c->win[0].size = 0x400000; c->win[0].name = "natural";
        c->win[1].p = b + 0x4400000; c->win[1].size = 0x400000; c->win[1].mask = 1; c->win[1].name = "word";
        c->win[2].p = b + 0x4000000; c->win[2].size = 0x400000; c->win[2].mask = 3; c->win[2].name = "long";
        break;
    case K_CV643D_Z2:
        c->io = b + 0x3c0000;
        c->ioxor = 3;
        c->nwin = 1;
        c->win[0].p = b; c->win[0].size = 0x3a0000; c->win[0].name = "natural";
        break;
    case K_A2410:
        c->regs = b;              /* host interface registers at the board base */
        break;
    case K_ZZ9000:
        c->regs = b;
        c->nwin = 1;
        if (c->memsize < 0x10000UL) return 0;
        c->win[0].p = b + 0x10000; c->win[0].size = c->memsize - 0x10000; c->win[0].name = "natural";
        if (c->win[0].size > (c->bus == 3 ? 0x400000UL : 0x200000UL))
            c->win[0].size = c->bus == 3 ? 0x400000UL : 0x200000UL;
        break;
    default:
        break;
    }
    return 1;
}

/* [p, p+len) inside the memory board or the register board */
static int in_board(const struct rtg_card *c, volatile UBYTE *p, ULONG len)
{
    ULONG a = (ULONG)p;
    if (c->mem && a >= (ULONG)c->mem && a - (ULONG)c->mem <= c->memsize &&
        len <= c->memsize - (a - (ULONG)c->mem))
        return 1;
    if (c->regs && a >= (ULONG)c->regs && a - (ULONG)c->regs <= c->regsize &&
        len <= c->regsize - (a - (ULONG)c->regs))
        return 1;
    return 0;
}

/* every offset the suites use must lie inside the autoconfig size of the
 * board it is on (review H1: a too-small board, e.g. a CV64/3D Z3 whose
 * layout reaches $C0E0008, or Picasso IV Z2 registers at +$10000).
 * Returns 0 and the first offset that does not fit. */
static int layout_fits(const struct rtg_card *c, const char **what)
{
    int i;
    ULONG iospan = (c->chip == CH_S3TRIO || c->chip == CH_S3VIRGE) ? 0x10000UL   /* 8514 ports to $FEE8 */
                 : c->chip == CH_ET4000 ? 0x217bUL                                /* W32 index port 217A */
                 : 0x3e0UL;                                                       /* VGA ports */
    for (i = 0; i < c->nwin; i++)
        if (!in_board(c, c->win[i].p, c->win[i].size)) { *what = c->win[i].name; return 0; }
    if (c->nwin && c->win[0].size < RTG_WORK) { *what = "work area (64 KB of VRAM)"; return 0; }
    if (c->io && !in_board(c, c->io, iospan)) { *what = "VGA I/O ports"; return 0; }
    switch (c->kind) {
    case K_PICASSO2: if (!in_board(c, c->regs, 0xb001)) { *what = "board registers $9000-$B000"; return 0; } break;
    case K_PICCOLO: if (!in_board(c, c->regs, 0x8001)) { *what = "board register $8000"; return 0; } break;
    case K_CV64: if (!in_board(c, c->mem, 0x40002)) { *what = "board control $40001"; return 0; } break;
    case K_CV643D_Z3: if (!in_board(c, c->mem, 0xc0e0008)) { *what = "PCI config $C0E0000 / MMIO"; return 0; } break;
    case K_CV643D_Z2: if (!in_board(c, c->mem, 0x3d0000)) { *what = "board registers $3A0000-$3CFFFF"; return 0; } break;
    case K_A2410: if (!in_board(c, c->regs, 8)) { *what = "host interface"; return 0; } break;
    case K_ZZ9000: if (!in_board(c, c->regs, 0x10000)) { *what = "register block"; return 0; } break;
    default: break;
    }
    return 1;
}

static int claim_card(const struct rtg_card *c)
{
    return c->mem_board && amt_board_claim(c->mem_board) &&
           (!c->reg_board || amt_board_claim(c->reg_board));
}

static void run_card(struct rtg_card *c)
{
    const char *what = 0;
    rtg_cur = c;
    amt_case_open(rtg_cid("detect"), AMT_ST_PASS);
    json_obj("ctx");
    json_str("name", c->name);
    json_int("man", (long)c->man);
    json_int("prod", (long)c->prod_mem);
    json_hex("mem", (ULONG)c->mem);
    json_int("mem_size", (long)c->memsize);
    if (c->regs && c->kind != K_PICASSO4_Z3 && c->kind != K_ZZ9000) {
        json_hex("regs", (ULONG)c->regs);
        json_int("regs_size", (long)c->regsize);
    }
    json_obj_end();
    amt_case_close();

    switch (c->kind) {
    case K_UAEGFX:
        amt_case("uaegfx", AMT_ST_UNTESTABLE,
                 "UAE Zorro II/III RTG (uaegfx) has no hardware register interface: the Picasso96 driver talks to the emulator through traps only");
        return;
    case K_OTHER:
        rtg_skip("suite", "board recognised, but this module has no register-level suite for its chip yet; nothing is written to it");
        return;
    default:
        break;
    }
    if (c->kind != K_A2410 && c->kind != K_ZZ9000 && c->kind != K_PICASSO4_Z3 && c->kind != K_CV64 &&
        c->kind != K_CV643D_Z2 && c->kind != K_CV643D_Z3 && !c->regs) {
        rtg_skip("suite", "the register board of this card is missing or was rejected by the board guard; nothing is written");
        return;
    }
    if (!setup_card(c)) {
        rtg_skip("suite", "board size/layout is not independently verified; nothing is accessed");
        return;
    }
    if (!layout_fits(c, &what)) {
        amt_case_open(rtg_cid("suite"), AMT_ST_SKIP);
        json_str("why", "the board is smaller than the register/VRAM layout of this card needs; nothing is written");
        json_obj("ctx"); json_str("needs", what); json_int("mem_size", (long)c->memsize);
        json_int("regs_size", (long)c->regsize); json_obj_end();
        amt_case_close();
        return;
    }
    if ((c->chip == CH_TMS34010 || c->chip == CH_ZZ9000) && !c->regs) {
        rtg_skip("suite", "internal: no register base for this board");
        return;
    }
    if (c->chip != CH_TMS34010 && c->chip != CH_ZZ9000 && !c->io) {
        rtg_skip("suite", "internal: no I/O base for this board");
        return;
    }
    if (!claim_card(c)) {
        rtg_skip("suite", "exclusive live ConfigDev claim failed for memory or register board; unsafe consent cannot override a driver or missing provenance");
        return;
    }
    switch (c->chip) {
    case CH_CIRRUS: rtg_cirrus(); break;
    case CH_S3TRIO:
    case CH_S3VIRGE: rtg_s3(); break;
    case CH_ET4000: rtg_et4000(); break;
    case CH_TMS34010: rtg_a2410(); break;
    case CH_ZZ9000: rtg_zz9000(); break;
    default: break;
    }
}

void amt_run_rtg(void)
{
    int i;
    UBYTE *pool = (UBYTE *)amt_any_pool();
    rtg_save = pool;
    rtg_buf = pool + RTG_WORK;
    ncards = 0;
    rtg_cur = 0;
    find_cards();
    for (i = 0; i < nrej; i++) {
        struct rtg_card tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.id = rejected[i]->id;
        rtg_cur = &tmp;
        amt_case_open(rtg_cid("board_rejected"), AMT_ST_SKIP);
        json_str("why", "the expansion list has this card, but its ConfigDev is shut up or not at a "
                        "valid Zorro II/III address (amt_board_ok): it is never accessed");
        json_obj("ctx"); json_hex("addr", rej_addr[i]); json_hexn("cd_flags", rej_flags[i], 2); json_obj_end();
        amt_case_close();
        rtg_cur = 0;
    }
    if (!ncards && nrej)
        return;
    if (!ncards) {
        amt_case("no_card", AMT_ST_SKIP, "no graphics card this module recognises in the expansion board list");
        return;
    }
    for (i = 0; i < ncards; i++)
        run_card(&cards[i]);
    rtg_cur = 0;
}
