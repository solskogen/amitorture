/* expsound: sound cards at register level (CONTRACT.md section 10, id 17,
 * disk group "exp").
 *
 * Boards are taken ONLY from amt_probe_result.boards (expansion.library's
 * ConfigDev list) and only when manufacturer/product are known:
 *   MacroSystem Toccata      man 18260 ($4754) prod 12  AD1848 codec
 *   ACT Prelude (Zorro II)   man 16945 ($4231) prod 1   CS4231A codec
 *   UAE uaesnd (Z2/Z3)       man 6502          prod 2   UAE register ABI
 * The Prelude 1200 sits on the A1200 clock port at $D80000 and is not an
 * autoconfig board, so it is probed only with AMT_F_UNSAFE (run parameter
 * flags bit 0): one index write and two reads at $D80001/$D80005.
 *
 * The codec expectations come from the AD1848K and CS4231A data sheets
 * (index/data register protocol, MCE protection of I8/I9/I28, ACI, the
 * read-only ID/version bits of I12/I25) and from the probing sequence of
 * the OSS/Linux ad1848 driver, which encodes those data-sheet facts.
 * The board glue (register addresses, the Toccata status register, the
 * Prelude FIFO flag bits) has no public documentation; its layout is
 * taken from Amiberry's sndboard.cpp, the flag polarity is the IDT720x
 * FIFO convention (active-low /EF, /HF, /FF). uaesnd is a UAE-only
 * device whose interface is specified in the comment block of
 * sndboard.cpp; that specification is the expectation source.
 * See docs/cases/expsound.md. */
#include "mod.h"
#include "cia_hw.h"
#include "expsound_hw.h"

AMT_MODULE(expsound, 17, AMT_PH_BARE, 0, 0, 0);

#define MAN_MACROSYSTEM 18260
#define PROD_TOCCATA    12
#define MAN_ACT         16945
#define PROD_PRELUDE    1
#define MAN_UAE         6502
#define PROD_UAESND     2

#define INT_EXTER 0x2000

static ULONG frames(void) { return amt_frames_now(); }

/* ======================================================= codec boards */
enum { CB_TOCCATA, CB_PRELUDE, CB_P1200 };

struct cb {
    const char *n;
    int kind;
    int cs;                       /* 1: CS4231(A), mode 2 capable */
    ULONG base;
    volatile UBYTE *r0, *r1, *fifo, *stat, *rst;
    UBYTE hf, ef;                 /* Prelude FIFO status: play /HF, play /EF bits */
    UBYTE fmask;                  /* the 4 FIFO flag bits */
    UBYTE fshift;
};

static struct cb *B;
static UBYTE mce;
static int host_audio = 1;        /* cleared after the first unacknowledged audio_mark */

static void cx(UBYTE i) { es_wb(B->r0, (UBYTE)(mce | i)); }
static UBYTE cr(UBYTE i)
{
    cx(i);
    return es_rb(B->r1);
}
static void cw(UBYTE i, UBYTE v)
{
    cx(i);
    es_wb(B->r1, v);
}
static void cmce(int on)
{
    mce = on ? 0x40 : 0;
    es_wb(B->r0, mce);
}
/* R0 bit 7 INIT: the codec ignores accesses while it is set */
static int cinit_wait(ULONG to_frames)
{
    ULONG t0 = frames();
    while (es_rb(B->r0) & 0x80)
        if (frames() - t0 > to_frames)
            return 0;
    return 1;
}
/* I11 bit 5 ACI: autocalibration in progress */
static int caci_wait(ULONG to_frames)
{
    ULONG t0 = frames();
    while (cr(11) & 0x20)
        if (frames() - t0 > to_frames)
            return 0;
    return 1;
}
/* end a mode change: clear MCE, wait for INIT and ACI to clear */
static void cmce_off(void)
{
    cmce(0);
    cinit_wait(50);
    caci_wait(100);
}

static const char *pc(const char *s) { return es_cid(B->n, s); }

static void ctx_board(void)
{
    json_obj("ctx");
    json_str("board", B->n);
    json_hex("base", B->base);
    json_str("codec", B->cs ? "CS4231A" : "AD1848");
    /* review M6: the board glue (register offsets, status bits, FIFO flags,
     * interrupt model) is not from a hardware document */
    json_str("glue_source", "board glue modelled on Amiberry sndboard.cpp (no public board documentation); "
                            "codec registers from the AD1848K/CS4231A data sheets");
    json_obj_end();
}

static void rep(const char *id, int ok, const char *why, const char *what,
                ULONG e, ULONG g, int dig, const char *in)
{
    if (ok) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_hexn(what, e, dig);
    json_obj_end();
    json_obj("got");
    json_hexn(what, g, dig);
    json_obj_end();
    json_str("why", why);
    if (in) {
        json_obj("in");
        json_str("seq", in);
        json_obj_end();
    }
    ctx_board();
    amt_case_close();
}

/* ------------------------------------------------ register file tests */
static UBYTE sav[16];

static void t_index(void)
{
    static const UBYTE modes[3] = { 0x00, 0x40, 0x20 };   /* -, MCE, TRD */
    int i, m, bad = -1;
    UBYTE w = 0, g = 0;
    for (m = 0; m < 3 && bad < 0; m++) {
        /* MCE 1 -> 0 starts the codec's resynchronisation (R0 bit 7 INIT,
         * then autocalibration if I9 ACAL): R0 reads $80 and writes are
         * ignored until it ends (AD1848K/CS4231A data sheets, MCE).
         * Leave MCE through that wait before the TRD group (review M7). */
        if (m == 2) {
            mce = 0x40;
            cmce_off();
        }
        for (i = 0; i < 16; i++) {
            w = (UBYTE)(modes[m] | i);
            es_wb(B->r0, w);
            g = es_rb(B->r0);
            if ((g ^ w) & 0x6f) {
                bad = i;
                break;
            }
        }
    }
    mce = 0x40;            /* leave MCE through the normal path */
    cmce_off();
    sb_reset();
    sb_s("R0 <- ");
    sb_h(w, 2);
    sb_s(" then read R0; compared bits $6F (MCE, TRD, IXA3-0)");
    rep(pc("index_readback"), bad < 0,
        "the index address register R0 must read back MCE (bit 6), TRD (bit 5) and IXA3-0 as written "
        "(AD1848/CS4231 data sheet, R0)", "r0", w, g, 2, es_sb);
}

struct rw { UBYTE idx, v; };
/* values keep reserved bits 0 (I0/I1 bit 4, I2-I5 bits 6-5, I6/I7 bit 6,
 * I13 bit 1); the output registers keep their mute bit set */
static const struct rw rwtab[] = {
    { 0, 0xaa }, { 1, 0x45 }, { 0, 0x45 }, { 1, 0xaa },
    { 2, 0x9f }, { 3, 0x88 }, { 4, 0x88 }, { 5, 0x9f },
    { 6, 0x9f }, { 7, 0xa5 }, { 13, 0xfc }, { 14, 0x5a }, { 15, 0xa5 },
};

static void t_ireg(void)
{
    unsigned k;
    int bad = -1;
    UBYTE g = 0;
    for (k = 0; k < sizeof(rwtab) / sizeof(rwtab[0]); k++) {
        cw(rwtab[k].idx, rwtab[k].v);
        g = cr(rwtab[k].idx);
        if (g != rwtab[k].v) {
            bad = (int)k;
            break;
        }
    }
    for (k = 0; k < 8; k++)
        cw((UBYTE)k, sav[k]);
    cw(13, sav[13] & 0xfe);
    cw(14, sav[14]);
    cw(15, sav[15]);
    sb_reset();
    if (bad >= 0) {
        sb_s("I");
        sb_d(rwtab[bad].idx);
        sb_s(" <- ");
        sb_h(rwtab[bad].v, 2);
        sb_s(" then read I");
        sb_d(rwtab[bad].idx);
    }
    rep(pc("ireg_readback"), bad < 0,
        "indirect registers I0-I7, I13-I15 are read/write: a value with the reserved bits 0 must read "
        "back unchanged (data sheet register map; same probe as the OSS ad1848 driver)",
        "value", bad >= 0 ? rwtab[bad].v : 0, g, 2, bad >= 0 ? es_sb : 0);
}

static void t_id(void)
{
    UBYTE id = cr(12), id2;
    UBYTE lo = id & 0x0f;
    int ok = B->cs ? lo == 0x0a : (lo == 0x0a || lo == 0x01);
    if (ok)
        amt_case(pc("codec_id"), AMT_ST_PASS, 0);
    else {
        amt_case_open(pc("codec_id"), AMT_ST_FAIL);
        json_obj("expect");
        json_str("i12_low_nibble", B->cs ? "$a" : "$a (rev C) or $1 (rev B)");
        json_obj_end();
        json_obj("got");
        json_hexn("i12", id, 2);
        json_obj_end();
        json_str("why", "I12 bits 3-0 are the codec ID: $A on the CS4231(A) and AD1848 rev C, $1 on AD1848 rev B");
        ctx_board();
        amt_case_close();
    }
    /* ID bits are read-only; writing the complement must not change them
     * (keeps bit 6 = 0: stays in mode 1) */
    cw(12, (UBYTE)(~id & 0x0f));
    id2 = cr(12);
    rep(pc("codec_id_readonly"), ((id ^ id2) & 0x0f) == 0,
        "I12 bits 3-0 (codec ID/revision) are read-only: writing their complement must not change them",
        "i12_low_nibble", id & 0x0f, id2 & 0x0f, 1, "I12 <- ~ID & $0F, read I12");
    if (!B->cs) {
        /* AD1848 has no mode 2: I12 bit 6 is read-only 0 */
        cw(12, 0x40);
        id2 = cr(12);
        rep(pc("no_mode2"), (id2 & 0x40) == 0,
            "the AD1848 has no MODE2 bit: I12 bit 6 is read-only and reads 0 after writing $40 "
            "(OSS ad1848 driver uses this to tell an AD1848 from a CS4231)",
            "i12_bit6", 0, id2 & 0x40, 2, "I12 <- $40, read I12");
        cw(12, 0x00);
    }
}

static void t_i8(void)
{
    static const UBYTE fmts[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x5b };
    int k, bad = -1;
    UBYTE w = 0, g = 0;
    cmce(1);
    for (k = 0; k < 16 + (int)sizeof(fmts); k++) {
        w = k < 16 ? (UBYTE)k : fmts[k - 16];
        cw(8, w);
        g = cr(8);
        if ((g ^ w) & 0x7f) {
            bad = k;
            break;
        }
    }
    cw(8, 0x00);
    cmce_off();
    sb_reset();
    sb_s("R0 MCE=1; I8 <- ");
    sb_h(w, 2);
    sb_s(" then read I8 (all 16 CSL/CFS sample rates, then formats)");
    rep(pc("i8_rates_formats"), bad < 0,
        "with MCE set, I8 (clock source CSL, divider CFS2-0, S/M, C/L, FMT) is read/write: every one of "
        "the 16 sample-rate selections and the format bits must read back as written",
        "i8", w, g, 2, es_sb);

    /* MCE clear: I8 writes are ignored */
    cw(8, 0x5b);
    g = cr(8);
    rep(pc("i8_needs_mce"), (g & 0x7f) == 0x00,
        "I8 cannot be changed unless MCE is set (AD1848/CS4231 data sheet, R0 MCE: 'the Data Format (I8) "
        "and Interface Configuration (I9) registers CANNOT be changed unless this bit is set')",
        "i8", 0x00, g, 2, "MCE=1, I8 <- $00, MCE=0, I8 <- $5B (MCE=0), read I8");

    /* MCE clear: I9 bits other than PEN/CEN are ignored */
    cw(9, 0x04);
    g = cr(9);
    rep(pc("i9_needs_mce"), (g & 0xcc) == 0x00,
        "I9 bits other than PEN/CEN cannot be changed unless MCE is set (data sheet R0 MCE; PEN and CEN "
        "are the only on-the-fly bits)", "i9", 0x00, g & 0xcc, 2,
        "MCE=1, I9 <- $00, MCE=0, I9 <- $04 (SDC, MCE=0), read I9 & $CC");
    cmce(1);
    cw(9, 0x00);
    cmce_off();
}

static void t_autocal(void)
{
    ULONG t0, t1, t2;
    int seen = 0, cleared = 0;
    cmce(1);
    cw(8, 0x01);                    /* 5512.5 Hz: a long calibration */
    cw(9, 0x08);                    /* ACAL */
    cmce(0);
    t0 = frames();
    t1 = t2 = 0;
    while (frames() - t0 < 10) {    /* up to 0.2 s for ACI to rise */
        if (cr(11) & 0x20) {
            seen = 1;
            t1 = frames() - t0;
            break;
        }
    }
    if (seen) {
        while (frames() - t0 < 150) {
            if (!(cr(11) & 0x20)) {
                cleared = 1;
                t2 = frames() - t0;
                break;
            }
        }
    }
    cinit_wait(50);
    cmce(1);
    cw(9, 0x00);
    cw(8, 0x00);
    cmce_off();
    if (seen && cleared)
        amt_case(pc("autocal_aci"), AMT_ST_PASS, 0);
    else {
        amt_case_open(pc("autocal_aci"), AMT_ST_FAIL);
        json_obj("expect");
        json_str("aci", "1 within 0.2 s of MCE 1->0, then 0 within 3 s");
        json_obj_end();
        json_obj("got");
        json_bool("aci_seen", seen);
        json_bool("aci_cleared", cleared);
        json_obj_end();
        json_str("why", "with I9 ACAL=1, clearing MCE starts autocalibration and I11 bit 5 (ACI) reads 1 "
                 "until it finishes (data sheet I9 ACAL / I11 ACI; Linux snd_wss_mce_down waits for it)");
        json_obj("in");
        json_str("seq", "MCE=1, I8 <- $01, I9 <- $08 (ACAL), MCE=0, poll I11");
        json_obj_end();
        ctx_board();
        amt_case_close();
    }
    (void)t1;
    (void)t2;
}

/* CS4231(A) mode 2 */
static void t_mode2(void)
{
    UBYTE g, i25, s16, s0, s18, s19, s26, s28;
    cw(12, 0x40);
    g = cr(12);
    rep(pc("mode2_enable"), (g & 0xc0) == 0xc0 && (g & 0x0f) == 0x0a,
        "writing MODE2 (I12 bit 6) on a CS4231 sets it; bit 7 reads 1 and bits 3-0 keep ID $A "
        "(CS4231 data sheet I12; OSS ad1848 detection 'tmp1 & 0xc0 == 0xc0')",
        "i12", 0xca, g, 2, "I12 <- $40, read I12 (compared bits $CF)");
    if (!(g & 0x40)) {
        amt_case(pc("i25_version"), AMT_ST_UNTESTABLE, "MODE2 could not be enabled (I12 bit 6 reads 0)");
        amt_case(pc("i25_readonly"), AMT_ST_UNTESTABLE, "MODE2 could not be enabled (I12 bit 6 reads 0)");
        amt_case(pc("i16_separate"), AMT_ST_UNTESTABLE, "MODE2 could not be enabled (I12 bit 6 reads 0)");
        amt_case(pc("mode2_regs"), AMT_ST_UNTESTABLE, "MODE2 could not be enabled (I12 bit 6 reads 0)");
        amt_case(pc("i28_needs_mce"), AMT_ST_UNTESTABLE, "MODE2 could not be enabled (I12 bit 6 reads 0)");
        cw(12, 0x00);
        return;
    }
    i25 = cr(25);
    if ((i25 & 0xe7) == 0xa0 || (i25 & 0xe7) == 0x80)
        amt_case(pc("i25_version"), AMT_ST_PASS, 0);
    else {
        amt_case_open(pc("i25_version"), AMT_ST_FAIL);
        json_obj("expect");
        json_str("i25_and_e7", "$a0 (CS4231A) or $80 (CS4231)");
        json_obj_end();
        json_obj("got");
        json_hexn("i25", i25, 2);
        json_obj_end();
        json_str("why", "I25 (version / chip ID): bits 7-5 version, 2-0 chip id; CS4231A reads $A0, CS4231 $80");
        ctx_board();
        amt_case_close();
    }
    cw(25, (UBYTE)~i25);
    g = cr(25);
    cw(25, i25);
    rep(pc("i25_readonly"), ((g ^ i25) & 0xe7) == 0,
        "I25 version/ID bits ($E7) are read-only: writing the complement must not change them "
        "(CS4231 data sheet I25; OSS ad1848 detection step)",
        "i25_and_e7", i25 & 0xe7, g & 0xe7, 2, "I25 <- ~I25, read I25");

    s16 = cr(16);
    s0 = cr(0);
    cw(16, 0x00);
    cw(0, 0x45);
    g = cr(16);
    cw(0, s0);
    cw(16, s16);
    rep(pc("i16_separate"), (g & 0xf3) == 0x00,
        "in MODE2 index 16 is its own register (Alternate Feature Enable I), not an alias of I0: "
        "writing I0 must not change I16 (OSS CS4231 detection: I16 vs I0)",
        "i16", 0x00, g, 2, "MODE2, I16 <- $00, I0 <- $45, read I16 (compared bits $F3)");

    s18 = cr(18);
    s19 = cr(19);
    s26 = cr(26);
    {
        static const struct rw m2[] = { { 18, 0x9f }, { 19, 0x88 }, { 26, 0xc5 }, { 18, 0x88 }, { 19, 0x9f } };
        unsigned k;
        int bad = -1;
        g = 0;
        for (k = 0; k < sizeof(m2) / sizeof(m2[0]); k++) {
            cw(m2[k].idx, m2[k].v);
            g = cr(m2[k].idx);
            if (g != m2[k].v) {
                bad = (int)k;
                break;
            }
        }
        cw(18, s18);
        cw(19, s19);
        cw(26, s26);
        sb_reset();
        if (bad >= 0) {
            sb_s("MODE2, I");
            sb_d(m2[bad].idx);
            sb_s(" <- ");
            sb_h(m2[bad].v, 2);
        }
        rep(pc("mode2_regs"), bad < 0,
            "MODE2 registers I18/I19 (line input) and I26 (mono I/O) are read/write with their "
            "reserved bits 0 (CS4231 data sheet)", "value", bad >= 0 ? m2[bad].v : 0, g, 2,
            bad >= 0 ? es_sb : 0);
    }

    s28 = cr(28);
    cmce(1);
    cw(28, 0x00);
    cmce_off();
    cw(28, 0x50);
    g = cr(28);
    rep(pc("i28_needs_mce"), (g & 0xf0) == 0x00,
        "I28 (capture data format) cannot be changed unless MCE is set (CS4231 data sheet, R0 MCE)",
        "i28", 0x00, g & 0xf0, 2, "MODE2, MCE=1, I28 <- $00, MCE=0, I28 <- $50, read I28 & $F0");
    cmce(1);
    cw(28, s28 & 0xf0);
    cmce_off();

    cw(12, 0x00);
    g = cr(12);
    rep(pc("mode1_back"), (g & 0x40) == 0,
        "clearing I12 bit 6 returns the CS4231 to MODE1", "i12_bit6", 0, g & 0x40, 2, "I12 <- $00");
}

/* ------------------------------------------------------- FIFO playback */
struct fmt {
    const char *tag;
    UBYTE i8;
    UBYTE bps;            /* bytes per sample frame */
    ULONG rate2;          /* sample rate x2 (5512.5 Hz = 11025) */
    ULONG bytes, qbytes;  /* buffer size, quick mode */
};
static const struct fmt fmts[2] = {
    { "u8m_5512", 0x01, 1, 11025, 4096, 2048 },   /* CSL=1 (16.9344 MHz) CFS=0 (/3072) */
    { "s16s_11025", 0x53, 4, 22050, 8192, 4096 }, /* CSL=1 CFS=1 (/1536), 16-bit LE stereo */
};

#define FIFO_SIZE  1024
#define FIFO_HALF  512

static const struct fmt *pf;
static volatile ULONG pb_pos, pb_left, pb_irq, pb_spur, pb_t0, pb_t1, pb_done;
static volatile ULONG irq_entries, irq_storm;

/* storm guard: a level-6 source that cannot be acknowledged would starve
 * VERTB (and the watchdog); give up after this many hook entries */
#define STORM_LIMIT 20000
static int storm_check(void)
{
    if (++irq_entries > STORM_LIMIT) {
        CREG(R_INTENA) = INT_EXTER;
        irq_storm = 1;
        return 1;
    }
    return 0;
}

static UBYTE pb_gen(ULONG i)
{
    if (pf->bps == 1)
        return ((i >> 4) & 1) ? 0x98 : 0x68;        /* square wave, 172 Hz */
    {
        ULONG fr = i >> 2;
        UWORD v = ((fr / 50) & 1) ? 0x0c00 : (UWORD)-0x0c00;   /* 220 Hz */
        return (i & 1) ? (UBYTE)(v >> 8) : (UBYTE)v;
    }
}

/* the whole stream is generated before playback; the interrupt handler only
 * copies (a 68000 must refill 512 bytes well within one half-FIFO period) */
#define PB_MAX 8192
static UBYTE pbuf[PB_MAX];

static void pb_prepare(ULONG total)
{
    ULONG i;
    for (i = 0; i < total; i++)
        pbuf[i] = pb_gen(i);
}

static void pb_fill(ULONG n)
{
    const UBYTE *src = pbuf + pb_pos;
    volatile UBYTE *f = B->fifo;
    if (n > pb_left)
        n = pb_left;
    pb_pos += n;
    pb_left -= n;
    while (n--)
        es_wb(f, *src++);
}

static void pb_stamp(void)
{
    ULONG t = cia_now();
    if (!pb_irq)
        pb_t0 = t;
    pb_t1 = t;
    pb_irq++;
}

/* Toccata: board status read; bit 7 = 0: board interrupt, bit 3: play FIFO half */
static void hook_toccata(ULONG pending)
{
    if (storm_check())
        return;
    if (pending & INT_EXTER) {
        UBYTE st = es_rb(B->stat);
        if (!(st & 0x80) && (st & 0x08)) {
            pb_stamp();
            if (pb_left)
                pb_fill(FIFO_HALF);
            else {
                es_wb(B->stat, 0x15);    /* no data left: PLAY_INTENA off, */
                (void)es_rb(B->stat);    /* then drop a request latched meanwhile */
                pb_done = 1;
            }
        } else
            pb_spur++;
        CREG(R_INTREQ) = INT_EXTER;
    }
}

/* Prelude: the interrupt is level: asserted while the play FIFO is below
 * half. Refill, or with no data left mask EXTER until the FIFO drains. */
static void hook_prelude(ULONG pending)
{
    if (storm_check())
        return;
    if (pending & INT_EXTER) {
        UBYTE st = es_rb(B->stat);
        if (st & B->hf) {                /* /HF high: below half */
            pb_stamp();
            if (pb_left)
                pb_fill(FIFO_HALF);
            else {
                CREG(R_INTENA) = INT_EXTER;
                pb_done = 1;
            }
        } else
            pb_spur++;
        CREG(R_INTREQ) = INT_EXTER;
    }
}

static void board_reset(void)
{
    if (B->kind == CB_TOCCATA) {
        es_wb(B->stat, 0x02);        /* RESET */
        es_wb(B->stat, 0x00);
    } else
        es_wb(B->rst, 0x00);         /* reset strobe: FIFOs and codec */
    cinit_wait(50);
    mce = 0;
}

/* The start mark describes the tone for the host's analysis (CONTRACT
 * section 13; the runner's host.tone_<tag> pseudo case): frequency in mHz,
 * tolerance 2 %, and how long it plays (ms of samples). */
static char am_args[96];
static char *am_cat(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static int audio_mark(const char *tag, int start, ULONG freq_mhz, ULONG ms)
{
    char *p = am_args;
    if (!host_audio)
        return 0;
    if (start) {
        p = am_cat(p, "{\"edge\":\"start\",\"freq_mhz\":");
        p = amt_utoa(p, freq_mhz);
        p = am_cat(p, ",\"tol_pct\":2,\"ms\":");
        p = amt_utoa(p, ms);
        am_cat(p, "}");
    } else
        am_cat(p, "{\"edge\":\"stop\"}");
    if (!amt_request("audio_mark", tag, am_args,
                     /* the host answers "stop" after a 0.35 s settle and the
                      * analysis (~0.8 s host time); 1 s timed out on a loaded
                      * host. 10 s like the other modules' audio marks. */
                     amt_secs(10))) {
        host_audio = 0;
        return 0;
    }
    return 1;
}

static void play(int fi, int intena, int mark)
{
    const struct fmt *f = &fmts[fi];
    ULONG total = !intena ? FIFO_SIZE : AMT_QUICK() ? f->qbytes : f->bytes;
    ULONG expect = intena ? (total - FIFO_SIZE) / FIFO_HALF + 1 : 0;
    ULONG period_cck = (FIFO_HALF / f->bps) * 2UL * amt_cck_hz() / f->rate2;
    ULONG dur_frames = (total / f->bps) * 2 * 50 / f->rate2 + 1;
    ULONG t0, to;
    int completed, drained = 1, marked = 0;
    char tag[40];
    const char *ccid;

    pf = f;
    pb_pos = pb_irq = pb_spur = pb_t0 = pb_t1 = pb_done = 0;
    irq_entries = irq_storm = 0;
    pb_left = total;
    pb_prepare(total);

    board_reset();
    cmce(1);
    cw(8, f->i8);
    cw(9, 0x00);
    cmce_off();
    cw(6, 0x08);                       /* DAC L/R unmuted, -12 dB */
    cw(7, 0x08);
    if (B->kind == CB_TOCCATA) {
        es_wb(B->stat, 0x01);          /* ACTIVE: FIFO reset */
        es_wb(B->stat, 0x15);          /* ACTIVE | FIFO_CODEC | FIFO_PLAY */
    }
    pb_fill(FIFO_SIZE);
    CREG(R_INTREQ) = INT_EXTER;
    amt_irq_hook[6] = B->kind == CB_TOCCATA ? hook_toccata : hook_prelude;
    CREG(R_INTENA) = 0x8000 | INT_EXTER;
    if (B->kind == CB_TOCCATA && intena)
        es_wb(B->stat, 0x95);          /* + PLAY_INTENA */

    {
        char *d = tag;
        const char *s = B->n;
        while (*s)
            *d++ = *s++;
        *d++ = '_';
        s = f->tag;
        while (*s)
            *d++ = *s++;
        *d = 0;
    }
    if (mark)
        /* u8m: a square wave of 32 frames per cycle (pb_gen); s16s: 100 */
        marked = audio_mark(tag, 1, f->rate2 * 500UL / (f->bps == 1 ? 32UL : 100UL),
                            (total / f->bps) * 2000UL / f->rate2);

    cw(9, 0x01);                       /* PEN: on the fly, no MCE needed */
    t0 = frames();
    to = dur_frames * 3 + 100;
    while (frames() - t0 < to) {
        if (pb_left == 0 && pb_irq >= expect && (pb_done || !intena))
            break;
    }
    completed = pb_left == 0 && pb_irq >= expect;
    /* let the FIFO drain: 2 interrupt periods + 4 frames */
    {
        ULONG w = (FIFO_SIZE / f->bps) * 2 * 50 / f->rate2 + 4;
        ULONG t1 = frames();
        while (frames() - t1 < w)
            ;
    }
    if (B->kind != CB_TOCCATA) {
        ULONG t1 = frames();
        drained = 0;
        while (frames() - t1 < 50)
            if (!(es_rb(B->stat) & B->ef)) {   /* /EF low: empty */
                drained = 1;
                break;
            }
    }
    cw(9, 0x00);
    CREG(R_INTENA) = INT_EXTER;
    amt_irq_hook[6] = 0;
    if (mark && marked)
        marked = audio_mark(tag, 0, 0, 0);
    cw(6, 0x80);
    cw(7, 0x80);
    board_reset();
    CREG(R_INTREQ) = INT_EXTER;

    if (!intena) {
        ccid = pc("play_intena_off");
        if (pb_irq == 0 && pb_spur == 0 && !irq_storm)
            amt_case(ccid, AMT_ST_PASS, 0);
        else {
            amt_case_open(ccid, AMT_ST_FAIL);
            json_obj("expect");
            json_int("irqs", 0);
            json_int("spurious", 0);
            json_obj_end();
            json_obj("got");
            json_int("irqs", (long)pb_irq);
            json_int("spurious", (long)pb_spur);
            json_obj_end();
            json_str("why", "with PLAY_INTENA (status bit 7) clear the board must not raise INT6 while the "
                     "play FIFO drains");
            ctx_board();
            amt_case_close();
        }
        return;
    }

    {
        char sfx[32];
        char *d = sfx;
        const char *s = "play_";
        while (*s)
            *d++ = *s++;
        s = f->tag;
        while (*s)
            *d++ = *s++;
        *d = 0;
        ccid = pc(sfx);
    }
    if (completed && pb_irq == expect && pb_spur == 0 && drained && !irq_storm)
        amt_case(ccid, AMT_ST_PASS, 0);
    else {
        amt_case_open(ccid, AMT_ST_FAIL);
        json_obj("expect");
        json_int("irqs", (long)expect);
        json_int("spurious", 0);
        json_int("undelivered", 0);
        json_bool("drained", 1);
        json_obj_end();
        json_obj("got");
        json_int("irqs", (long)pb_irq);
        json_int("spurious", (long)pb_spur);
        json_int("undelivered", (long)pb_left);
        json_bool("drained", drained);
        json_int("hook_entries", (long)irq_entries);
        json_bool("storm", irq_storm);
        json_obj_end();
        json_str("why", "play FIFO (1024 bytes) filled, then refilled with 512 bytes at every half-empty "
                 "interrupt (the last one, with no data left, masks the interrupt): a buffer of N bytes "
                 "gives exactly (N-1024)/512+1 interrupts at level 6 (INT6) and the FIFO then drains");
        json_obj("in");
        json_int("buffer_bytes", (long)total);
        json_hexn("i8", f->i8, 2);
        json_int("bytes_per_frame", f->bps);
        json_int("rate_x2", (long)f->rate2);
        json_obj_end();
        json_obj("ctx");
        json_str("board", B->n);
        json_hex("base", B->base);
        json_obj("timing");
        json_int("first_to_last_cck", (long)(pb_t1 - pb_t0));
        json_int("expect_period_cck", (long)period_cck);
        json_obj_end();
        json_obj_end();
        amt_case_close();
    }
    if (pb_irq > 1) {
        char pn[48];
        char *d = pn;
        const char *s = tag;
        while (*s)
            *d++ = *s++;
        s = "_irq_period";
        while (*s)
            *d++ = *s++;
        *d = 0;
        /* ops = interrupt intervals, ticks = colour clocks; one interval is
         * 512 bytes / bytes-per-frame / sample rate */
        amt_perf(pn, pb_irq - 1, pb_t1 - pb_t0);
    }
    if (mark) {
        ccid = pc("audio_capture");
        if (marked)
            amt_case(ccid, AMT_ST_PASS, 0);
        else
            amt_case(ccid, AMT_ST_UNTESTABLE, "host did not act on request audio_mark");
    }
}

/* Prelude FIFO flags (active low /HF, /EF for play, /HF, /FF for record) */
static void t_fifo_flags(void)
{
    static const UBYTE expn[5] = { 0xb, 0xf, 0x7, 0x7, 0xb };
    static const ULONG lvl[5] = { 0, 1, 600, 1024, 0 };
    UBYTE got[5];
    int k, ok = 1;
    pf = &fmts[0];
    pb_prepare(FIFO_SIZE);
    board_reset();
    pb_pos = 0;
    got[0] = (UBYTE)((es_rb(B->stat) & B->fmask) >> B->fshift);
    for (k = 1; k < 4; k++) {
        pb_left = lvl[k] - lvl[k - 1];
        pb_fill(pb_left);
        got[k] = (UBYTE)((es_rb(B->stat) & B->fmask) >> B->fshift);
    }
    board_reset();
    got[4] = (UBYTE)((es_rb(B->stat) & B->fmask) >> B->fshift);
    CREG(R_INTREQ) = INT_EXTER;
    for (k = 0; k < 5; k++)
        if (got[k] != expn[k])
            ok = 0;
    if (ok)
        amt_case(pc("fifo_flags"), AMT_ST_PASS, 0);
    else {
        amt_case_open(pc("fifo_flags"), AMT_ST_FAIL);
        json_arr("expect");
        for (k = 0; k < 5; k++)
            json_hexn(0, expn[k], 1);
        json_arr_end();
        json_arr("got");
        for (k = 0; k < 5; k++)
            json_hexn(0, got[k], 1);
        json_arr_end();
        json_str("why", "FIFO status nibble (/HF play, /EF play, /HF rec, /FF rec; active low) after a "
                 "reset, 1, 600, 1024 bytes written to the idle play FIFO, and after another reset");
        json_obj("in");
        json_str("levels", "0,1,600,1024,reset");
        json_hexn("status_mask", B->fmask, 2);
        json_obj_end();
        ctx_board();
        amt_case_close();
    }
}

static struct cia_saved cia_b;

static void codec_board(struct cb *b)
{
    int k;
    UBYTE r0;
    B = b;
    mce = 0;
    if (!cinit_wait(50)) {
        amt_case_open(pc("init_bit_clear"), AMT_ST_FAIL);
        json_obj("expect");
        json_str("r0_bit7", "0 within 1 s");
        json_obj_end();
        json_obj("got");
        json_hexn("r0", es_rb(b->r0), 2);
        json_obj_end();
        json_str("why", "R0 bit 7 (INIT) stays set: the codec never finished initialising, every other "
                 "codec case would be meaningless");
        ctx_board();
        amt_case_close();
        return;
    }
    r0 = es_rb(b->r0);
    rep(pc("init_bit_clear"), (r0 & 0x80) == 0, "R0 bit 7 (INIT) must read 0 once the codec is idle",
        "r0_bit7", 0, r0 & 0x80, 2, 0);
    for (k = 0; k < 16; k++)
        sav[k] = cr((UBYTE)k);
    {
        /* initial register file, for diagnosis (informational) */
        char t[80];
        char *d = t;
        static const char hx[] = "0123456789abcdef";
        for (k = 0; k < 16; k++) {
            *d++ = hx[sav[k] >> 4];
            *d++ = hx[sav[k] & 15];
        }
        *d = 0;
        json_begin("log");
        json_str("mod", "expsound");
        json_str("board", b->n);
        json_str("msg", "codec I0-I15 as found");
        json_str("regs", t);
        json_end();
    }
    cmce(1);
    cw(9, 0x00);                       /* PEN/CEN/ACAL off */
    cmce_off();

    t_index();
    t_ireg();
    t_id();
    t_i8();
    t_autocal();
    if (b->cs)
        t_mode2();
    amt_case(pc("record_loopback"), AMT_ST_SKIP,
             "AD1848/CS4231 have no digital DAC-to-ADC loopback (only the analog post-mix input, "
             "I0/I1 source 11), so no exact data compare is possible");

    cia_save(&cia_b, CIAB_BASE, 0);    /* CIA-B also drives INT6 */
    if (b->kind != CB_TOCCATA)
        t_fifo_flags();
    play(0, 1, 1);
    play(1, 1, 0);
    if (b->kind == CB_TOCCATA)
        play(0, 0, 0);
    cia_restore(&cia_b);

    /* restore the codec as found (never leave PEN/CEN on) */
    board_reset();
    cmce(1);
    cw(8, sav[8]);
    cw(9, sav[9] & ~0x03);
    cmce_off();
    for (k = 0; k < 8; k++)
        cw((UBYTE)k, sav[k]);
    cw(13, sav[13]);
    cw(14, sav[14]);
    cw(15, sav[15]);
    es_wb(b->r0, 0);
}

/* ======================================================= uaesnd */
static ULONG U;              /* board base */
static ULONG UR;             /* RAM base */
static int uz3;
static ULONG ucaps;

#define UB(o) ((volatile UBYTE *)(U + (o)))
#define UW(o) ((volatile UWORD *)(U + (o)))
#define UL(o) ((volatile ULONG *)(U + (o)))
#define S0 0x100              /* stream 1 */

static volatile ULONG u_irq, u_spur, u_t0, u_t1;
static volatile UBYTE u_seq[8];

static void hook_uae(ULONG pending)
{
    if (storm_check())
        return;
    if (pending & INT_EXTER) {
        UBYTE v = es_rb(UB(S0 + 0x87));   /* reading clears */
        if (v & 0x80) {
            ULONG t = cia_now();
            if (!u_irq)
                u_t0 = t;
            u_t1 = t;
            if (u_irq < 8)
                u_seq[u_irq] = v;
            u_irq++;
        } else
            u_spur++;
        CREG(R_INTREQ) = INT_EXTER;
    }
}

static void urep(const char *id, int ok, const char *why, const char *what, ULONG e, ULONG g, const char *in)
{
    if (ok) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_hex(what, e);
    json_obj_end();
    json_obj("got");
    json_hex(what, g);
    json_obj_end();
    json_str("why", why);
    if (in) {
        json_obj("in");
        json_str("seq", in);
        json_obj_end();
    }
    json_obj("ctx");
    json_str("board", uz3 ? "uaesnd_z3" : "uaesnd_z2");
    json_hex("base", U);
    json_obj_end();
    amt_case_close();
}

static void u_wait_frames(ULONG n)
{
    ULONG t0 = frames();
    while (frames() - t0 < n)
        ;
}

/* sample set structure (40 bytes) at RAM offset `so` */
static ULONG u_set(ULONG so, ULONG data, LONG len, ULONG freq, ULONG rep, LONG replen,
                   UWORD repcnt, UWORD flags, UBYTE intr)
{
    volatile UBYTE *p = (volatile UBYTE *)(UR + so);
    int i;
    for (i = 0; i < 48; i += 4)
        es_wl((volatile ULONG *)(p + i), 0);
    es_ww((volatile UWORD *)(p + 0), flags);
    es_ww((volatile UWORD *)(p + 2), 0);
    es_wl((volatile ULONG *)(p + 4), data);
    es_wl((volatile ULONG *)(p + 8), (ULONG)len);
    es_wl((volatile ULONG *)(p + 12), freq);
    es_wl((volatile ULONG *)(p + 16), rep);
    es_wl((volatile ULONG *)(p + 20), (ULONG)replen);
    es_ww((volatile UWORD *)(p + 24), repcnt);
    es_ww((volatile UWORD *)(p + 26), 0x2000);   /* volume 8192/32768 */
    es_wl((volatile ULONG *)(p + 28), 0);         /* next: none */
    es_wb(p + 32, 1);                             /* mono */
    es_wb(p + 33, 0x40);                          /* 8-bit signed */
    es_wb(p + 34, intr);
    return UR + so;
}

static void u_seq_case(const char *id, const UBYTE *exp, int n, UBYTE mask, const char *why, ULONG setaddr)
{
    int k, ok = (int)u_irq == n && u_spur == 0 && !irq_storm;
    for (k = 0; ok && k < n; k++)
        if ((u_seq[k] & mask) != exp[k])
            ok = 0;
    if (ok) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_int("irqs", n);
    json_hexn("status_mask", mask, 2);
    json_arr("status_masked");
    for (k = 0; k < n; k++)
        json_hexn(0, exp[k], 2);
    json_arr_end();
    json_obj_end();
    json_obj("got");
    json_int("irqs", (long)u_irq);
    json_int("spurious", (long)u_spur);
    json_bool("storm", irq_storm);
    json_arr("status");
    for (k = 0; k < (int)u_irq && k < 8; k++)
        json_hexn(0, u_seq[k], 2);
    json_arr_end();
    json_obj_end();
    json_str("why", why);
    json_obj("in");
    json_hex("set", setaddr);
    json_obj_end();
    json_obj("ctx");
    json_str("board", uz3 ? "uaesnd_z3" : "uaesnd_z2");
    json_hex("base", U);
    if (ucaps & 4) {
        json_int("invalid_set_count", (long)es_rl(UL(0xa0)));
        json_int("start_count", (long)es_rl(UL(0xa8)));
        json_int("last_error", (long)es_rl(UL(0xb8)));
    }
    json_hexn("status_193", es_rb(UB(S0 + 0x93)), 2);
    json_hex("setptr_180", es_rl(UL(S0 + 0x80)));
    json_obj_end();
    amt_case_close();
}

static void u_play_and_wait(ULONG setaddr, ULONG endbits, ULONG to_frames)
{
    ULONG t0;
    u_irq = u_spur = 0;
    irq_entries = irq_storm = 0;
    CREG(R_INTREQ) = INT_EXTER;
    amt_irq_hook[6] = hook_uae;
    CREG(R_INTENA) = 0x8000 | INT_EXTER;
    /* the spec does not say whether the $F0 enable bit survives the end of
     * a stream (Amiberry clears it): enable it again for every start */
    es_wl(UL(0xf0), 0x00000001);
    es_wl(UL(S0 + 0x80), setaddr);           /* set pointer: starts the stream */
    t0 = frames();
    while (frames() - t0 < to_frames) {
        if (u_irq && (u_seq[(u_irq - 1) & 7] & endbits) == endbits)
            break;
    }
    u_wait_frames(5);
    CREG(R_INTENA) = INT_EXTER;
    amt_irq_hook[6] = 0;
    CREG(R_INTREQ) = INT_EXTER;
}

static void uaesnd_board(const struct amt_probe_board *bd)
{
    ULONG v, roff, rsize, a, k, setaddr;
    ULONG d_start = 0, d_stop = 0, d_inv = 0;
    int ok;

    U = bd->addr;
    uz3 = bd->size > 0x10000;

    /* ---- read-only information block */
    v = es_rb(UB(0x8e));
    urep("uaesnd_info_streams", v == 8, "$008E.B (max simultaneous streams) is 8 (uaesnd spec, sndboard.cpp)",
         "streams", 8, v, 0);
    roff = es_rl(UL(0x90));
    rsize = es_rl(UL(0x94));
    a = uz3 ? 0x800000 : 0x8000;
    urep("uaesnd_info_ram", roff == a && rsize == a,
         uz3 ? "Z3 board: 8 MB RAM in the upper half: $0090.L offset $800000, $0094.L size $800000"
             : "Z2 board: 32 KB RAM at $8000-$FFFF: $0090.L offset $8000, $0094.L size $8000",
         "offset_size", a, roff == a ? rsize : roff, 0);
    ucaps = es_rl(UL(0x98));
    json_begin("log");
    json_str("mod", "expsound");
    json_str("msg", "uaesnd information block");
    json_hex("uae_version", es_rl(UL(0x80)));
    json_hexn("hw_version", es_rw(UW(0x84)), 4);
    json_hexn("hw_revision", es_rw(UW(0x86)), 4);
    json_int("freq", (long)es_rl(UL(0x88)));
    json_int("max_ch", es_rb(UB(0x8c)));
    json_int("active_ch", es_rb(UB(0x8d)));
    json_hex("caps", ucaps);
    json_end();
    if (roff != a || rsize != a) {
        amt_case("uaesnd_ram_rw", AMT_ST_UNTESTABLE, "RAM offset/size not as specified");
        return;
    }
    if (roff + rsize > bd->size) {
        amt_case("uaesnd_ram_rw", AMT_ST_UNTESTABLE, "the RAM block lies beyond the board's autoconfig size: not written");
        return;
    }
    UR = U + roff;

    /* ---- board RAM */
    {
        static const ULONG offs[4] = { 0, 0x1000, 0x4000, 0x7ff0 };
        ULONG bad_o = 0, e = 0, g = 0;
        ok = 1;
        for (k = 0; k < 4 && ok; k++) {
            volatile ULONG *p = (volatile ULONG *)(UR + offs[k]);
            ULONG i;
            for (i = 0; i < 4; i++)
                es_wl(p + i, 0xa5000000UL ^ (offs[k] << 4) ^ (i * 0x01010101UL));
            for (i = 0; i < 4; i++) {
                e = 0xa5000000UL ^ (offs[k] << 4) ^ (i * 0x01010101UL);
                g = es_rl(p + i);
                if (g != e) {
                    ok = 0;
                    bad_o = offs[k] + i * 4;
                    break;
                }
            }
        }
        sb_reset();
        sb_s("RAM+");
        sb_h(bad_o, 6);
        urep("uaesnd_ram_rw", ok, "board RAM is plain read/write memory (uaesnd spec)", "long", e, g, ok ? 0 : es_sb);
    }

    /* ---- allocation and enable masks */
    es_wl(UL(0xe0), 0x00000003);
    v = es_rl(UL(0xe0));
    es_wl(UL(0xe0), 0);
    a = es_rl(UL(0xe0));
    es_wb(UB(0xe3), 0x01);
    k = es_rl(UL(0xe0));
    urep("uaesnd_alloc_mask", v == 3 && a == 0 && k == 1,
         "$00E0.L allocated-streams mask is read/write; byte access to $00E3 is allowed (uaesnd spec)",
         "e0_after_3_0_e3b1", 0x00030001, (v << 16) | (a << 8) | k, "$E0.L <- 3, 0; $E3.B <- 1");
    /* stream 1 allocated from here on */
    es_wl(UL(0xf4), 0x00000001);
    v = es_rl(UL(0xf4));
    urep("uaesnd_master_inten", v == 1, "$00F4.L stream master interrupt enable mask is read/write",
         "f4", 1, v, "$F4.L <- 1");
    es_wl(UL(0xf0), 0x00000001);
    v = es_rl(UL(0xf0));
    urep("uaesnd_stream_enable", v == 1, "$00F0.L stream enable mask is read/write", "f0", 1, v, "$F0.L <- 1");

    ok = 1;
    for (k = 0; k < 0x40 && ok; k += 4) {
        es_wl(UL(S0 + 0x40 + k), 0x5a000000UL | (k * 0x10101));
        if (es_rl(UL(S0 + 0x40 + k)) != (0x5a000000UL | (k * 0x10101)))
            ok = 0;
    }
    for (k = 0; k < 0x40; k += 4)
        es_wl(UL(S0 + 0x40 + k), 0);
    urep("uaesnd_latch_rw", ok, "$0140-$017F (latched sample set structure) is read/write (uaesnd spec)",
         "ok", 1, ok, 0);

    cia_save(&cia_b, CIAB_BASE, 0);

    /* ---- timer interrupt: 200 Hz, status $90 (bit 7 + bit 4)
     * The timer value is a frequency in Hz*256 of emulated time: the period
     * is (colour clock rate)/200 colour clocks (Amiberry sndboard.cpp
     * uaesnd_timer_period(): base_event_clock*CYCLE_UNIT*256/v, with
     * base_event_clock = the colour clocks per second). The first interrupt
     * comes one period after the write. The window is a whole number of
     * frames (100 PAL, 120 NTSC, ~2 s), started and stopped right after a
     * VBL, so the count is floor(window/period): PAL 100*71051 cck /
     * (3546895/200) = 400.6 -> 400; NTSC 120*59832.5 cck / (3579545/200) =
     * 401.2 -> 401 (227.5 cck per line on average, HRM). No margin: the
     * count only changes if an edge moves by the distance to the nearest
     * interrupt, >= 0.37 period (~29 lines) PAL and >= 0.16 period (~12
     * lines) NTSC, while each edge lands within a line of its VBL (the
     * busy-wait sees the VBL counter change, then writes). A +-1 margin
     * would hide a rate error of up to 0.25 %. */
    {
        ULONG t0, wf, fr, ex;
        int sok = 1, n;
        if (amt_probe_result.pal) {
            wf = 100;
            fr = 71051UL * 2;                 /* half colour clocks per frame */
        } else {
            wf = 120;
            fr = 59833UL * 2 - 1;             /* 263 * 227.5 * 2 */
        }
        /* floor(wf * frame * 200 / cck_hz), in half colour clocks
         * (< 2^32: 2.84e9 PAL, 2.87e9 NTSC) */
        ex = (wf * fr * 200UL) / (amt_cck_hz() * 2UL);
        u_irq = u_spur = 0;
        irq_entries = irq_storm = 0;
        es_wb(UB(S0 + 0x8f), 0x10);           /* stream master int enable: timer */
        CREG(R_INTREQ) = INT_EXTER;
        amt_irq_hook[6] = hook_uae;
        CREG(R_INTENA) = 0x8000 | INT_EXTER;
        t0 = frames();
        while (frames() == t0)
            ;
        t0 = frames();
        es_wl(UL(S0 + 0x94), 200UL << 8);
        while (frames() - t0 < wf)
            ;
        es_wl(UL(S0 + 0x94), 0);
        n = (int)u_irq;
        u_wait_frames(10);
        CREG(R_INTENA) = INT_EXTER;
        amt_irq_hook[6] = 0;
        CREG(R_INTREQ) = INT_EXTER;
        for (k = 0; k < 8 && k < (ULONG)n; k++)
            if (u_seq[k] != 0x90)
                sok = 0;
        if (n == (int)ex && (int)u_irq == n && sok && u_spur == 0)
            amt_case("uaesnd_timer_irq", AMT_ST_PASS, 0);
        else {
            amt_case_open("uaesnd_timer_irq", AMT_ST_FAIL);
            json_obj("expect");
            json_int("irqs_running", (long)ex);
            json_int("period_cck", (long)(amt_cck_hz() / 200UL));
            json_str("irqs_total", "= irqs_running");
            json_str("rule", "floor(window / period) interrupts while the timer runs, none after it is set to 0");
            json_hexn("status", 0x90, 2);
            json_obj_end();
            json_obj("got");
            json_int("irqs_running", n);
            json_int("irqs_total", (long)u_irq);
            json_int("spurious", (long)u_spur);
            json_hexn("status0", u_seq[0], 2);
            json_obj_end();
            json_str("why", "$0194.L timer frequency (Hz*256) raises interrupt status bit 4 at that rate; "
                     "with $018F.B bit 4 and $00F4.L bit 0 set it reaches INT6, $0187.B reads $90 "
                     "(bit 7 active + bit 4); 0 disables the timer (uaesnd spec)");
            json_obj("in");
            json_str("seq", "$E0.L=1, $F4.L=1, $018F.B=$10, $0194.L=200<<8 right after a VBL, "
                     "$0194.L=0 right after the window's last VBL");
            json_int("window_frames", (long)wf);
            json_int("frame_half_cck", (long)fr);
            json_int("cck_hz", (long)amt_cck_hz());
            json_obj_end();
            /* mean interval first..last interrupt, beam-based colour clocks
             * (cia_now: exact PAL, 227 cck lines assumed NTSC); timing, so
             * not fingerprinted */
            json_obj("ctx");
            json_obj("timing");
            if (u_irq > 1)
                json_int("period_cck", (long)((u_t1 - u_t0) / (u_irq - 1)));
            json_obj_end();
            json_obj_end();
            amt_case_close();
        }
        if (u_irq > 1)
            amt_perf("uaesnd_timer_200hz_period", u_irq - 1, u_t1 - u_t0);
    }

    /* ---- sample data in board RAM: 8-bit signed square wave */
    for (k = 0; k < 4410; k++)
        es_wb((volatile UBYTE *)(UR + 0x1000 + k), ((k / 25) & 1) ? 0x20 : 0xe0);

    if (ucaps & 4) {
        d_start = es_rl(UL(0xa8));
        d_stop = es_rl(UL(0xac));
        d_inv = es_rl(UL(0xa0));
    }

    /* ---- one-shot playback: start and end interrupts */
    {
        static const UBYTE exp2[2] = { 0x81, 0x82 };
        int marked = 0;
        setaddr = u_set(0x100, UR + 0x1000, 4410, 22050UL << 8, 0, 0, 0, 0, 0x03);
        es_wb(UB(S0 + 0x8f), 0x03);
        /* 4410 frames at 22050 Hz (200 ms), 50 frames per cycle: 441 Hz */
        marked = audio_mark("uaesnd_play", 1, 441000UL, 200);
        u_play_and_wait(setaddr, 0x82, 150);
        if (marked)
            marked = audio_mark("uaesnd_play", 0, 0, 0);
        u_seq_case("uaesnd_play_irqs", exp2, 2, 0x87,
                   "a one-shot sample set (34.B = start|end) gives exactly two interrupts: $0187.B "
                   "bits 7+0 when the set starts, bits 7+1 when it ends (status masked with $87; uaesnd spec)",
                   setaddr);
        if (u_irq >= 2)
            amt_perf("uaesnd_play_4410_22050_start_to_end", 4410, u_t1 - u_t0);
        v = es_rb(UB(S0 + 0x93));
        urep("uaesnd_play_stopped", (v & 1) == 0,
             "after the last set ends the stream stops: $0193.B bit 0 (play active) reads 0", "status_193", 0,
             v & 1, 0);
        v = es_rl(UL(S0 + 0x80));
        urep("uaesnd_setptr_cleared", v == 0,
             "the sample set pointer $0180.L 'also clears automatically when stream ends' (uaesnd spec)",
             "setptr", 0, v, 0);
        if (ucaps & 4) {
            ULONG s1 = es_rl(UL(0xa8)) - d_start, s2 = es_rl(UL(0xac)) - d_stop;
            urep("uaesnd_diag_start_stop", s1 == 1 && s2 == 1,
                 "diagnostics: one playback adds 1 to $00A8 (stream start count) and 1 to $00AC (stop count)",
                 "start_stop_delta", 0x00010001, (s1 << 16) | (s2 & 0xffff), 0);
        } else
            amt_case("uaesnd_diag_start_stop", AMT_ST_SKIP, "no diagnostics capability ($0098 bit 2)");
        amt_case("uaesnd_audio_capture", marked ? AMT_ST_PASS : AMT_ST_UNTESTABLE,
                 marked ? 0 : "host did not act on request audio_mark");
    }

    /* ---- repeats: two repeat-start interrupts, then the end */
    {
        static const UBYTE exp3[3] = { 0x84, 0x84, 0x82 };
        setaddr = u_set(0x200, UR + 0x1000, 2205, 22050UL << 8, UR + 0x1000, 1102, 2, 0, 0x06);
        es_wb(UB(S0 + 0x8f), 0x06);
        u_play_and_wait(setaddr, 0x82, 150);
        u_seq_case("uaesnd_repeat_irqs", exp3, 3, 0x86,
                   "repeat count 2: when the sample ends the repeat part is played twice, each start "
                   "raising bit 2 (34.B bit 2); then the set ends (bit 1). Status masked with $86: bits 0 "
                   "and 3 are set whenever their condition occurs, enabled or not (uaesnd spec)",
                   setaddr);
    }

    /* ---- invalid set (flags != 0) is rejected */
    {
        if (ucaps & 4)
            d_inv = es_rl(UL(0xa0));
        setaddr = u_set(0x300, UR + 0x1000, 2205, 22050UL << 8, 0, 0, 0, 1, 0x01);
        es_wb(UB(S0 + 0x8f), 0x01);
        u_play_and_wait(setaddr, 0x81, 10);
        v = es_rb(UB(S0 + 0x93));
        ok = u_irq == 0 && (v & 1) == 0;
        if (ucaps & 4) {
            ULONG di = es_rl(UL(0xa0)) - d_inv, le = es_rl(UL(0xb8));
            ok = ok && di == 1 && le == 1;
            urep("uaesnd_invalid_set", ok,
                 "flags (0.W) must be zero: a set with flags=1 is invalid, must not start (no interrupt, "
                 "$0193 bit 0 = 0), adds 1 to $00A0 and sets $00B8 last error = 1 (uaesnd spec)",
                 "irqs_play_invdelta_lasterr", 0x00000101,
                 (u_irq << 24) | ((v & 1) << 16) | ((di & 0xff) << 8) | (le & 0xff), 0);
        } else
            urep("uaesnd_invalid_set", ok,
                 "flags (0.W) must be zero: a set with flags=1 is invalid and must not start",
                 "irqs_play", 0, (u_irq << 8) | (v & 1), 0);
    }

    cia_restore(&cia_b);

    /* ---- leave the board idle */
    es_wl(UL(S0 + 0x94), 0);
    es_wl(UL(S0 + 0x80), 0);
    es_wb(UB(S0 + 0x8f), 0);
    es_wl(UL(0xf0), 0);
    es_wl(UL(0xf4), 0);
    es_wl(UL(0xe0), 0);
}

/* ======================================================= main */
static struct cb cbs;

static int p1200_present(void)
{
    volatile UBYTE *r0 = (volatile UBYTE *)0xd80001, *r1 = (volatile UBYTE *)0xd80005;
    UBYTE i, id;
    if (!(amt_par.flags & AMT_F_UNSAFE))
        return 0;
    es_wb(r0, 0x0c);
    i = es_rb(r0);
    id = es_rb(r1);
    return (i & 0x7f) == 0x0c && (id & 0x0f) == 0x0a;
}

/* review H1: a known board whose ConfigDev fails the shared guard (shut
 * up, unconfigured, address 0 or outside the Zorro windows) is reported and
 * never touched */
static int board_usable(const struct amt_probe_board *bd, const char *name)
{
    static char id[40];
    char *d = id;
    const char *p = name;
    if (amt_board_ok(bd))
        return 1;
    while (*p) *d++ = *p++;
    p = "_board_rejected";
    while (*p) *d++ = *p++;
    *d = 0;
    amt_case_open(id, AMT_ST_SKIP);
    json_str("why", "the board is in the expansion list, but its ConfigDev is shut up or not at a "
                    "valid Zorro II/III address: it is never accessed");
    json_obj("ctx");
    json_hex("addr", bd->addr);
    json_hex("size", bd->size);
    json_hexn("cd_flags", bd->flags, 2);
    json_obj_end();
    amt_case_close();
    return 0;
}

void amt_run_expsound(void)
{
    int k, found = 0;
    for (k = 0; k < amt_probe_result.n_boards && k < AMT_PROBE_BOARDS_MAX; k++) {
        const struct amt_probe_board *bd = &amt_probe_result.boards[k];
        if (bd->man == MAN_MACROSYSTEM && bd->prod == PROD_TOCCATA && !board_usable(bd, "toccata")) {
            found++;
        } else if (bd->man == MAN_ACT && bd->prod == PROD_PRELUDE && !board_usable(bd, "prelude")) {
            found++;
        } else if (bd->man == MAN_UAE && bd->prod == PROD_UAESND && !board_usable(bd, "uaesnd")) {
            found++;
        } else if (bd->man == MAN_MACROSYSTEM && bd->prod == PROD_TOCCATA && bd->size >= 0x10000) {
            struct cb *b = &cbs;
            b->n = "toccata";
            b->kind = CB_TOCCATA;
            b->cs = 0;
            b->base = bd->addr;
            b->r0 = (volatile UBYTE *)(bd->addr + 0x6001);
            b->r1 = (volatile UBYTE *)(bd->addr + 0x6801);
            b->fifo = (volatile UBYTE *)(bd->addr + 0x2000);
            b->stat = (volatile UBYTE *)(bd->addr + 0x0000);
            b->rst = 0;
            codec_board(b);
            found++;
        } else if (bd->man == MAN_ACT && bd->prod == PROD_PRELUDE && bd->size >= 0x10000) {
            struct cb *b = &cbs;
            b->n = "prelude";
            b->kind = CB_PRELUDE;
            b->cs = 1;
            b->base = bd->addr;
            b->r0 = (volatile UBYTE *)(bd->addr + 0x80);
            b->r1 = (volatile UBYTE *)(bd->addr + 0x82);
            b->fifo = (volatile UBYTE *)(bd->addr + 0x88);
            b->stat = (volatile UBYTE *)(bd->addr + 0x8c);
            b->rst = (volatile UBYTE *)(bd->addr + 0x8a);
            b->hf = 0x80;
            b->ef = 0x40;
            b->fmask = 0xf0;
            b->fshift = 4;
            codec_board(b);
            found++;
        } else if (bd->man == MAN_UAE && bd->prod == PROD_UAESND && bd->size >= 0x10000) {
            uaesnd_board(bd);
            found++;
        }
    }
    if (p1200_present()) {
        struct cb *b = &cbs;
        b->n = "prelude1200";
        b->kind = CB_P1200;
        b->cs = 1;
        b->base = 0xd80000;
        b->r0 = (volatile UBYTE *)0xd80001;
        b->r1 = (volatile UBYTE *)0xd80005;
        b->fifo = (volatile UBYTE *)0xd80011;
        b->stat = (volatile UBYTE *)0xd80015;
        b->rst = (volatile UBYTE *)0xd80015;
        b->hf = 0x08;
        b->ef = 0x04;
        b->fmask = 0x0f;
        b->fshift = 0;
        codec_board(b);
        found++;
    }
    if (!found)
        amt_case("no_board", AMT_ST_SKIP,
                 (amt_par.flags & AMT_F_UNSAFE)
                     ? "no Toccata/Prelude/uaesnd in the expansion list and no Prelude 1200 at $D80000"
                     : "no Toccata (18260/12), Prelude (16945/1) or uaesnd (6502/2) in the expansion list; "
                       "the Prelude 1200 (clock port, not autoconfig) is probed only with the unsafe flag");
}
