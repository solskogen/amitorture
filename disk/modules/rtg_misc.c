/* rtg: Tseng ET4000/ET4000W32 (oMniBus, Domino, Merlin), TI TMS34010
 * (A2410) and MNT ZZ9000 suites.
 * References: Tseng Labs ET4000 data book (KEY, segment select 3CD,
 * CR33), TI TMS34010 User's Guide (host interface: HSTADRL/H, HSTDATA,
 * HSTCTL; I/O registers at 0xC0000000), MNT ZZ9000 register map. */
#include "rtg.h"

/* ---------------------------------------------------- ET4000 */
static struct vga_state est;
static UBYTE seg, cr33, cr37, cr30, cr36, ix217a;
static int w32, cr37_set, et_stage;

static void et_run(void)
{
    struct rtg_card *c = rtg_cur;
    int i, bad = 0;
    UBYTE bw = 0, br = 0;
    volatile UBYTE *vram = c->win[0].p;

    vga_save_selectors(&est);
    et_stage = 8;
    vga_save(&est, 8, 9, 0x38);
    et_stage |= 1;
    vga_color_io();
    /* KEY: 03h to 3BF, then A0h to 3D8 enables the extended registers */
    vo(0x3bf, 0x03);
    vo(0x3d8, 0xa0);

    /* segment select 3CD: bits 3:0 write segment, 7:4 read segment, R/W */
    seg = vi(0x3cd);
    cr33 = CR_R(0x33);
    cr37 = CR_R(0x37);
    cr30 = CR_R(0x30);
    cr36 = CR_R(0x36);
    ix217a = vi(0x217a);
    et_stage |= 2;
    for (i = 0; i < 5; i++) {
        static const UBYTE v[5] = { 0x55, 0xaa, 0x00, 0xff, 0x3c };
        vo(0x3cd, v[i]);
        br = vi(0x3cd);
        if (br != v[i]) { bad = 1; bw = v[i]; break; }
    }
    vo(0x3cd, seg);
    if (!bad)
        rtg_pass("et4_segment");
    else {
        amt_case_open(rtg_cid("et4_segment"), AMT_ST_FAIL);
        json_obj("expect"); json_hexn("value", bw, 2); json_obj_end();
        json_obj("got"); json_hexn("value", br, 2); json_obj_end();
        json_str("why", "ET4000 segment select register 3CD (read/write segment) does not read back after the KEY sequence (Tseng ET4000 data book)");
        json_obj("ctx"); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
    {
        static const struct regm ext[] = { {0x33, 0x03} };
        rtg_regfile("et4_cr33", 0x3d4, ext, 1, "CR33 (extended start address bits 17:16) does not read back after the KEY sequence");
    }
    vga_std_tests();

    vga_set_640x480();
    vo(0x3cd, 0x00);
    /* ET4000/W32: CR36 bit 4 linear map, CR30 (address map comparator)
     * = 0, which is where these boards put their window (W32 data book;
     * board model). The W32 is told apart by its 217A index register,
     * which reads back (the ET4000AX does not decode it). */
    vo(0x217a, 0x5c);
    w32 = vi(0x217a) == 0x5c;
    vo(0x217a, ix217a);
    if (w32) {
        CR_W(0x30, 0x00);
        CR_W(0x36, (UBYTE)(cr36 | 0x10));
    }
    /* CR37 (memory configuration: bus width, DRAM type, size) describes how
     * the board is wired, not a chip rule (review M9). It is kept as found
     * when anything has programmed it, and always on the W32 (Merlin:
     * 2 MB, a different wiring). Only on an ET4000AX board of the 1 MB
     * family (oMniBus, Domino) whose CR37 still reads 0 (nothing has set it
     * up: the CPU then reaches only 64 KB) is it set to what those boards'
     * drivers program, a 32-bit bus of 256Kx4 DRAMs (bits 3,1:0 = 1,11)
     * *(board model)*; it is restored at the end. VRAM is saved after this
     * and restored before CR37 is, so both use the same cells. */
    cr37_set = !w32 && cr37 == 0 && c->vram_max == 0x100000;
    if (cr37_set)
        CR_W(0x37, 0x0b);
    vga_check_640x480("mode_640x480");
    vga_palette_load();
    json_begin("log");
    json_str("mod", "rtg");
    json_str("msg", "ET4000 memory configuration (CR37 as found; set only if it was 0 on a 1 MB ET4000AX board)");
    json_hexn("cr37", cr37, 2);
    json_int("cr37_set", cr37_set);
    json_int("w32", w32);
    json_end();
    for (i = 0; i < (int)(RTG_WORK / 4); i++)
        ((ULONG *)rtg_save)[i] = ((volatile ULONG *)vram)[i];
    et_stage |= 4;
    rtg_vram_tests();
    rtg_draw_test("mode_draw", 640, 100);
    vga_retrace_test();
    rtg_screenshot("mode_screenshot", rtg_cid("640x480"));
    vga_vint_test(0);

}

static void et_restore(void)
{
    int i;
    if (!et_stage) return;
    if (!(et_stage & 1)) {
        vga_restore_selectors(&est);
        return;
    }
    rtg_monitor_restore();
    rtg_vram_restore();
    if (et_stage & 4)
        for (i = 0; i < (int)(RTG_WORK / 4); i++)
            ((volatile ULONG *)rtg_cur->win[0].p)[i] = ((ULONG *)rtg_save)[i];
    vga_restore_std(&est);
    vo(0x3c2, est.misc | 1);
    if (et_stage & 2) {
        CR_W(0x33, cr33);
        if (w32) {
            CR_W(0x36, cr36);
            CR_W(0x30, cr30);
        }
        CR_W(0x37, cr37);
        vo(0x3cd, seg);
        vo(0x217a, ix217a);
    }
    vo(0x3c2, est.misc);
    vga_restore_selectors(&est);
}

void rtg_et4000(void)
{
    int code, cleanup;
    et_stage = w32 = cr37_set = 0;
    code = amt_try(et_run);
    cleanup = amt_try(et_restore);
    if (cleanup) amt_abort_fatal(cleanup);
    if (code) amt_abort(code);
}

/* ---------------------------------------------------- A2410 / TMS34010 */
/* host interface: word registers at board + 0 (HSTADRL), 2 (HSTADRH),
 * 4 (HSTDATA), 6 (HSTCTL) (HCS/HFS1/HFS0 decode of the A2410) */
#define HADRL (*(volatile UWORD *)(rtg_cur->regs + 0))
#define HADRH (*(volatile UWORD *)(rtg_cur->regs + 2))
#define HDATA (*(volatile UWORD *)(rtg_cur->regs + 4))
#define HCTL  (*(volatile UWORD *)(rtg_cur->regs + 6))

#define HST_HLT  0x8000
#define HST_LBL  0x2000
#define HST_INCR 0x1000
#define HST_INCW 0x0800

/* HSTCTL write without disturbing the TMS: keep HLT, NMIM and MSGIN,
 * INTIN = 0, INTOUT written 1 (writing 0 would clear a pending request) */
static void hctl(UWORD keep, UWORD bits)
{
    HCTL = (UWORD)((keep & 0x8207) | (bits & 0x3800) | 0x0080);
}

static UWORD tms_ctl, tms_l, tms_h, tms_save[32];
static ULONG tms_addr;
static int tms_stage, tms_pending;

static void tms_seek(ULONG bitaddr)
{
    HADRL = (UWORD)bitaddr;
    HADRH = (UWORD)(bitaddr >> 16);    /* LBL = 0: the prefetch follows this write */
}

static int tms_rw(const char *suffix, ULONG bitaddr, int n, const char *why)
{
    UWORD keep = HCTL, e = 0, g = 0;
    int i, bad = -1;
    if (n > 32) n = 32;
    hctl(keep, HST_INCR);
    tms_seek(bitaddr);
    for (i = 0; i < n; i++) tms_save[i] = HDATA;
    tms_addr = bitaddr;
    tms_pending = n;
    hctl(keep, HST_INCW);
    tms_seek(bitaddr);
    for (i = 0; i < n; i++) HDATA = (UWORD)(0x5a00 ^ (i * 0x0101) ^ (bitaddr >> 4));
    hctl(keep, HST_INCR);
    tms_seek(bitaddr);
    for (i = 0; i < n; i++) {
        UWORD v = HDATA, x = (UWORD)(0x5a00 ^ (i * 0x0101) ^ (bitaddr >> 4));
        if (v != x && bad < 0) { bad = i; e = x; g = v; }
    }
    hctl(keep, HST_INCW);
    tms_seek(bitaddr);
    for (i = 0; i < n; i++) HDATA = tms_save[i];
    tms_pending = 0;
    hctl(keep, keep);
    if (bad < 0) { rtg_pass(suffix); return 1; }
    amt_case_open(rtg_cid(suffix), AMT_ST_FAIL);
    json_obj("expect"); json_hexn("word", e, 4); json_obj_end();
    json_obj("got"); json_hexn("word", g, 4); json_obj_end();
    json_str("why", why);
    json_obj("in"); json_hex("bitaddr", bitaddr + (ULONG)bad * 16); json_int("words", n); json_obj_end();
    json_obj("ctx"); json_hexn("hstctl", keep, 4); rtg_ctx_card(); json_obj_end();
    amt_case_close();
    return 0;
}

static void tms_run(void)
{
    UWORD ctl = HCTL, l0, h0, l1, h1, c1;
    /* HLT is an observed ownership prerequisite, not evidence that a
     * running board violates reset behaviour: firmware may have started it. */
    if (ctl & HST_HLT)
        rtg_pass("tms_reset_halted");
    else {
        rtg_skip("tms_reset_halted", "TMS is running; reset history and software ownership are unknown, not a reset conformance failure");
        rtg_skip("tms_host", "TMS34010 is running (HLT = 0): the host port tests are not run, so the running TMS is not disturbed");
        return;
    }
    if (ctl & 0x4100) {
        rtg_skip("tms_host", "cache flush or NMI is pending; host controls cannot be changed and restored without side effects");
        return;
    }
    /* address registers: 16-bit read/write */
    l0 = HADRL; h0 = HADRH;
    tms_ctl = ctl; tms_l = l0; tms_h = h0;
    tms_stage = 1;
    hctl(ctl, 0);
    HADRL = 0x1230; HADRH = 0x00ab;
    l1 = HADRL; h1 = HADRH;
    HADRL = l0; HADRH = h0;
    if (l1 == 0x1230 && h1 == 0x00ab)
        rtg_pass("tms_host_addr");
    else {
        amt_case_open(rtg_cid("tms_host_addr"), AMT_ST_FAIL);
        json_obj("expect"); json_hexn("hstadrl", 0x1230, 4); json_hexn("hstadrh", 0x00ab, 4); json_obj_end();
        json_obj("got"); json_hexn("hstadrl", l1, 4); json_hexn("hstadrh", h1, 4); json_obj_end();
        json_str("why", "HSTADRL/HSTADRH (host address registers) do not read back what the host wrote (TMS34010 UG, host interface)");
        json_obj("ctx"); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
    /* HSTCTL bits 13:11 (LBL, INCR, INCW) read/write from the host */
    hctl(ctl, HST_LBL | HST_INCW);
    c1 = HCTL;
    hctl(ctl, 0);
    {
        UWORD c2 = HCTL;
        hctl(ctl, ctl);
        if ((c1 & 0x3800) == (HST_LBL | HST_INCW) && (c2 & 0x3800) == 0) {
            amt_case_open(rtg_cid("tms_host_ctl"), AMT_ST_PASS);
            json_obj("ctx"); json_int("halted", (ctl & HST_HLT) ? 1 : 0); json_obj_end();
            amt_case_close();
        } else {
            amt_case_open(rtg_cid("tms_host_ctl"), AMT_ST_FAIL);
            json_obj("expect"); json_hexn("bits", HST_LBL | HST_INCW, 4); json_hexn("cleared", 0, 4); json_obj_end();
            json_obj("got"); json_hexn("bits", c1 & 0x3800, 4); json_hexn("cleared", c2 & 0x3800, 4); json_obj_end();
            json_str("why", "HSTCTL LBL/INCR/INCW (bits 13:11) do not read back (TMS34010 UG)");
            json_obj("ctx"); json_hexn("hstctl", ctl, 4); rtg_ctx_card(); json_obj_end();
            amt_case_close();
        }
    }
    /* memory through the host port, 32 words with auto-increment:
     * frame buffer (TMS bit address 0x00010000), program DRAM
     * (0x01800000 + 0x70000 bits, near its end) only when the TMS is halted */
    tms_rw("tms_host_vram", 0x00010000UL, 32, "frame buffer words written through HSTDATA with INCW do not read back through HSTDATA with INCR (TMS34010 UG host interface; A2410 VRAM at TMS address 0)");
    tms_rw("tms_host_dram", 0x01800000UL + 0x7f0000UL, 32, "program DRAM words written through the host port do not read back (A2410 DRAM at TMS bit address 0x01800000)");
    /* I/O registers through the host port: CONVSP (0xC0000130), CONVDP
     * (0xC0000140), PMASK (0xC0000160), plain 16-bit read/write */
    {
        static const ULONG regs[3] = { 0xc0000130UL, 0xc0000140UL, 0xc0000160UL };
        UWORD keep = HCTL, v, e = 0, g = 0;
        int i, bad = -1;
        for (i = 0; i < 3; i++) {
            hctl(keep, 0);
            tms_seek(regs[i]);
            tms_save[0] = HDATA;
            tms_addr = regs[i];
            tms_pending = 1;
            tms_seek(regs[i]);
            HDATA = (UWORD)(0x1357 + i * 0x2222);
            tms_seek(regs[i]);
            v = HDATA;
            tms_seek(regs[i]);
            HDATA = tms_save[0];
            tms_pending = 0;
            if (v != (UWORD)(0x1357 + i * 0x2222) && bad < 0) { bad = i; e = (UWORD)(0x1357 + i * 0x2222); g = v; }
        }
        hctl(keep, keep);
        if (bad < 0)
            rtg_pass("tms_ioregs");
        else {
            amt_case_open(rtg_cid("tms_ioregs"), AMT_ST_FAIL);
            json_obj("expect"); json_hexn("value", e, 4); json_obj_end();
            json_obj("got"); json_hexn("value", g, 4); json_obj_end();
            json_str("why", "TMS34010 I/O register (CONVSP/CONVDP/PMASK) written through the host port does not read back (TMS34010 UG, I/O registers at 0xC0000000)");
            json_obj("in"); json_hex("bitaddr", regs[bad]); json_obj_end();
            json_obj("ctx"); rtg_ctx_card(); json_obj_end();
            amt_case_close();
        }
    }
}

static void tms_restore(void)
{
    int i;
    if (!tms_stage) return;
    if (!(HCTL & HST_HLT)) {
        rtg_chk("tms_restore", "halted", 1, 0, "TMS unexpectedly running: host memory restoration withheld");
        amt_abort_fatal(AMT_ABORT_UNEXPECTED);
    }
    if (tms_pending) {
        hctl(tms_ctl, HST_INCW);
        tms_seek(tms_addr);
        for (i = 0; i < tms_pending; i++) HDATA = tms_save[i];
        tms_pending = 0;
    }
    hctl(tms_ctl, 0);
    HADRL = tms_l; HADRH = tms_h;
    hctl(tms_ctl, tms_ctl);  /* final restoration after every memory test */
}

void rtg_a2410(void)
{
    int code, cleanup;
    tms_stage = tms_pending = 0;
    code = amt_try(tms_run);
    cleanup = amt_try(tms_restore);
    if (cleanup) amt_abort_fatal(cleanup);
    if (code) amt_abort(code);
}

/* ---------------------------------------------------- ZZ9000 */
#define ZZR(o) (*(volatile UWORD *)(rtg_cur->regs + (o)))

void rtg_zz9000(void)
{
    struct rtg_card *c = rtg_cur;
    UWORD fw = ZZR(0xc0), hw = ZZR(0x00);
    ULONG f0;
    int s0 = 0, s1 = 0, i;

    c->vram_max = c->win[0].size;
    if (fw != 0 && fw != 0xffff) {
        amt_case_open(rtg_cid("zz_fw_version"), AMT_ST_PASS);
        json_obj("ctx"); json_hexn("fw", fw, 4); json_hexn("hw", hw, 4); json_obj_end();
        amt_case_close();
    } else {
        amt_case_open(rtg_cid("zz_fw_version"), AMT_ST_FAIL);
        json_obj("expect"); json_str("fw", "not 0000/ffff"); json_obj_end();
        json_obj("got"); json_hexn("fw", fw, 4); json_obj_end();
        json_str("why", "ZZ9000 firmware version register (offset 0xC0) reads as no device");
        json_obj("ctx"); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
    /* Official firmware 1.14 returns zstate_raw bit 21 in this 32-bit
     * register; the Amiga upper word at +4C therefore uses bit 5. There
     * is no advertised capability bit for alternate firmware layouts. */
    if (fw != 0x010e) {
        rtg_skip("zz_vblank", "vblank encoding verified for official firmware 1.14 only; unknown/alternate firmware has no capability discriminator");
    } else {
    f0 = amt_frames_now();
    while (amt_frames_now() - f0 < 4 && !(s0 && s1)) {
        if (ZZR(0x4c) & 0x20) s1 = 1; else s0 = 1;
    }
    if (s0 && s1)
        rtg_pass("zz_vblank");
    else {
        amt_case_open(rtg_cid("zz_vblank"), AMT_ST_FAIL);
        json_obj("expect"); json_str("vblank", "toggles"); json_obj_end();
        json_obj("got"); json_int("seen0", s0); json_int("seen1", s1); json_obj_end();
        json_str("why", "ZZ9000 vblank status (upper word at 0x4C bit 5) did not toggle within 4 Amiga frames");
        json_obj("ctx"); rtg_ctx_card(); json_obj_end();
        amt_case_close();
    }
    }
    /* The memory window at +$10000 is the ZZ9000's ARM-side RAM, which the
     * running firmware uses (frame buffers, GFXData, its own state); there
     * is no documented free area in it. Neither a Zorro claim nor unsafe
     * consent establishes a firmware reservation, so write cases skip. */
    {
        static const char *const ids[] = { "vram_bwl", "vram_addr_lines", "vram_walk", "vram_draw" };
        for (i = 0; i < 4; i++)
            rtg_skip(ids[i], "ZZ9000 RAM belongs to running ARM firmware; unsafe consent does not reserve a race-free work area");
        return;
    }
}
