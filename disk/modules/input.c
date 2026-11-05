/* input: keyboard, mouse/joystick counters, pot ports, CD32 pad
 * (CONTRACT.md section 10, module id 15, disk group "chip").
 *
 * References: Amiga Hardware Reference Manual (HRM) 3rd ed., chapter 8
 * "Interface Hardware" (controller ports, keyboard), appendix A (JOYxDAT,
 * JOYTEST, POTGO/POTINP, POTxDAT), appendix F (8520). The CD32 pad protocol
 * is not in the HRM; it is the one lowlevel.library ReadJoyPort() uses (the
 * pad's 74xx165 shift register: pin 5 low selects shift mode, pin 6 /FIR is
 * the clock, pin 9 the data; 7 buttons, then a 1, then a 0).
 *
 * Input events the CPU cannot cause itself come through the host request
 * channel (CONTRACT section 13): key, mouse, joy, cd32pad. Every dependent
 * case is "untestable" when the host does not acknowledge the request. A
 * "wait" probe goes first: without an answer no further request is sent
 * (each would cost a timeout). After an acked request the module waits for
 * the event with a timeout and reports exact counts.
 *
 * Host-free cases (exact everywhere): JOYTEST read-back, POTGO output pin
 * read-back via POTINP; the pot counters are
 * reported as a timing field only (the values with nothing connected are
 * emulator/hardware specific).
 *
 * Safety: CIA-A is saved/restored with cia_hw.h (CRA/CRB/latches/ICR);
 * CIA-A DDRA bit 7 (/FIR1 as the CD32 clock output) is only set during a
 * CD32 read that the host acknowledged, and DDRA is restored; PRA writes keep
 * the LED bit and force OVL = 0. POTGO is left at $FF00 (all pot pins
 * outputs, high: what Kickstart 2.04+ and AROS write at boot; POTGO is
 * write-only). JOY0DAT is restored through JOYTEST at the end, so the OS
 * mouse pointer does not jump. Never injected: Ctrl-Amiga-Amiga (keys are
 * pressed one at a time, except one two-key rollover of A+S), caps lock,
 * codes $68-$7F (reset warning $78, CDTV keys). */
#include "mod.h"
#include "cia_hw.h"
#include "hardware_audit.h"

AMT_MODULE(input, 15, AMT_PH_BARE, AMT_MF_LONG, 0, 0);

#define R_JOY0DAT 0x00a
#define R_JOY1DAT 0x00c
#define R_POT0DAT 0x012
#define R_POT1DAT 0x014
#define R_POTINP  0x016
#define R_POTGO   0x034
#define R_JOYTEST 0x036

#define POTGO_IDLE 0xff00   /* all four pins output, high */

/* POTGO/POTINP pin bits (data; output enable = data << 1) */
#define P_LX 0x0100    /* port 0 pin 5 (middle mouse button) */
#define P_LY 0x0400    /* port 0 pin 9 (right mouse button) */
#define P_RX 0x1000    /* port 1 pin 5 (CD32 pad mode select) */
#define P_RY 0x4000    /* port 1 pin 9 (2nd fire / CD32 data) */

#define FIR0 0x40      /* CIA-A PRA: port 0 fire (left mouse button), low = pressed */
#define FIR1 0x80      /* CIA-A PRA: port 1 fire, low = pressed */

/* custom register access by explicit MOVE.W (no CLR, no read of write-only
 * registers) */
#define CRD(off) ({ UWORD _v; __asm__ volatile("move.w %1,%0" : "=d"(_v) \
        : "m"(*(volatile UWORD *)(0xdff000UL + (off))) : "memory"); _v; })
#define CWR(off, val) __asm__ volatile("move.w %1,%0" \
        : "=m"(*(volatile UWORD *)(0xdff000UL + (off))) : "d"((UWORD)(val)) : "memory")

static struct cia_saved sav_a;
static UBYTE saved_ddra;
static UWORD saved_joy0;

/* ------------------------------------------------------------ helpers */
static void wait_lines(ULONG n) { cia_wait_cc(n * 227UL); }

static void wait_frames(ULONG n)
{
    ULONG f0 = amt_frames_now();
    while (amt_frames_now() - f0 < n)
        ;
}

/* a few CIA accesses: ~1.4 us each (E clock), for CD32 pad settling */
static void cia_delay(int n)
{
    while (n-- > 0)
        (void)cia_rd(CIAA_BASE, C_PRA);
}

/* small JSON-args builder for amt_request */
static char abuf[96];
static char *ap;

static void a_begin(void)
{
    ap = abuf;
    *ap++ = '{';
}

static void a_key(const char *k)
{
    if (ap[-1] != '{')
        *ap++ = ',';
    *ap++ = '"';
    while (*k)
        *ap++ = *k++;
    *ap++ = '"';
    *ap++ = ':';
}

static void a_int(const char *k, long v)
{
    a_key(k);
    if (v < 0) {
        *ap++ = '-';
        v = -v;
    }
    ap = amt_utoa(ap, (ULONG)v);
}

static void a_str(const char *k, const char *s)
{
    a_key(k);
    *ap++ = '"';
    while (*s)
        *ap++ = *s++;
    *ap++ = '"';
}

static const char *a_end(void)
{
    *ap++ = '}';
    *ap = 0;
    return abuf;
}

/* ------------------------------------------------------------ host gating */
enum { H_WAIT, H_KEY, H_MOUSE, H_JOY, H_CD32, H_N };
static const char *const hname[H_N] = { "wait", "key", "mouse", "joy", "cd32pad" };
static const char *const hwhy[H_N] = {
    "host did not act on request wait",
    "host did not act on request key",
    "host did not act on request mouse",
    "host did not act on request joy",
    "host did not act on request cd32pad",
};
static UBYTE hdead[H_N];     /* a request of this kind went unacknowledged */
static UBYTE hnone;          /* the "wait" probe went unacknowledged */

/* 1 = acknowledged, 0 = sent and timed out, -1 = not sent (gated) */
static int req(int kind, const char *tag, const char *args)
{
    if (hnone || hdead[kind])
        return -1;
    if (!amt_request(hname[kind], tag, args, amt_secs(kind == H_WAIT ? 10 : 5))) {
        hdead[kind] = 1;
        if (kind == H_WAIT)
            hnone = 1;
        return 0;
    }
    return 1;
}

static void host_untestable(const char *id, int kind, int r)
{
    amt_case_open(id, AMT_ST_UNTESTABLE);
    json_str("why", hwhy[kind]);
    json_obj("ctx");
    json_str("request", hname[kind]);
    json_int("sent", r == 0);
    if (r < 0)
        json_str("note", hnone ? "the 'wait' probe was not acknowledged (no host request support): request not sent"
                               : "an earlier request of this kind was not acknowledged: not sent again");
    json_obj_end();
    amt_case_close();
}

/* ============================================================ JOYTEST */
static const UWORD jt_pat[] = {
    0x0000, 0xffff, 0xaaaa, 0x5555, 0xfcfc, 0x0404, 0x8000, 0x0080,
    0x1234, 0xc3a5, 0x7f80, 0x80fc,
};
#define N_JT (sizeof(jt_pat) / sizeof(jt_pat[0]))

static ULONG jt_hot(ULONG i)
{
    ULONG k, acc = 0;
    UWORD a, b;
    (void)i;
    for (k = 0; k < N_JT; k++) {
        CWR(R_JOYTEST, jt_pat[k]);
        a = CRD(R_JOY0DAT);
        b = CRD(R_JOY1DAT);
        acc = (acc << 3) ^ (acc >> 29) ^ ((ULONG)((a ^ jt_pat[k]) & 0xfcfc) << 16)
              ^ ((b ^ jt_pat[k]) & 0xfcfc);
    }
    return acc;
}

static void joytest_tests(void)
{
    UWORD pat[N_JT + 8];
    UWORD g0[N_JT + 8], g1[N_JT + 8], b0[N_JT + 8], b1[N_JT + 8];
    int n = 0, i, bad_hi = -1, bad_lo = -1;
    ULONG first, bad;
    long r;

    for (i = 0; i < (int)N_JT; i++)
        pat[n++] = jt_pat[i];
    for (i = 0; i < (AMT_QUICK() ? 2 : 8); i++)
        pat[n++] = (UWORD)amt_rand();

    for (i = 0; i < n; i++) {
        b0[i] = CRD(R_JOY0DAT);
        b1[i] = CRD(R_JOY1DAT);
        CWR(R_JOYTEST, pat[i]);
        g0[i] = CRD(R_JOY0DAT);
        g1[i] = CRD(R_JOY1DAT);
        if (bad_hi < 0 && (((g0[i] ^ pat[i]) | (g1[i] ^ pat[i])) & 0xfcfc))
            bad_hi = i;
        if (bad_lo < 0 && (((g0[i] ^ b0[i]) | (g1[i] ^ b1[i])) & 0x0303))
            bad_lo = i;
    }

    if (bad_hi < 0) {
        amt_case_open("joytest_readback", AMT_ST_PASS);
        json_obj("in");
        json_int("patterns", n);
        json_obj_end();
        amt_case_close();
    } else {
        i = bad_hi;
        amt_case_open("joytest_readback", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("joy0dat_and_fcfc", pat[i] & 0xfcfc, 4);
        json_hexn("joy1dat_and_fcfc", pat[i] & 0xfcfc, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("joy0dat", g0[i], 4);
        json_hexn("joy1dat", g1[i], 4);
        json_obj_end();
        json_obj("in");
        json_hexn("joytest", pat[i], 4);
        json_int("index", i);
        json_obj_end();
        json_str("why", "HRM A JOYTEST: a write sets bits 7-2 of all four mouse counters at once "
                 "(Y7-Y2 = data 15-10, X7-X2 = data 7-2); JOY0DAT/JOY1DAT must read them back");
        amt_case_close();
    }

    if (bad_lo < 0) {
        amt_case("joytest_low_bits_unchanged", AMT_ST_PASS, 0);
    } else {
        i = bad_lo;
        amt_case_open("joytest_low_bits_unchanged", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("joy0dat_and_0303", b0[i] & 0x0303, 4);
        json_hexn("joy1dat_and_0303", b1[i] & 0x0303, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("joy0dat", g0[i], 4);
        json_hexn("joy1dat", g1[i], 4);
        json_obj_end();
        json_obj("in");
        json_hexn("joytest", pat[i], 4);
        json_hexn("joy0dat_before", b0[i], 4);
        json_hexn("joy1dat_before", b1[i], 4);
        json_obj_end();
        json_str("why", "HRM A JOYTEST: bits 1-0 of each counter are not written (they follow the "
                 "quadrature inputs); with nothing moving they must not change");
        amt_case_close();
    }

    r = amt_hot(jt_hot, amt_hot_reps(), &first, &bad);
    if (r < 0 && first == 0)
        amt_case("joytest_hot", AMT_ST_PASS, 0);
    else {
        amt_case_open("joytest_hot", AMT_ST_FAIL);
        json_obj("expect");
        json_hex("signature", 0);
        json_obj_end();
        json_obj("got");
        json_hex("signature_first", first);
        if (r >= 0) {
            json_hex("signature_bad", bad);
            json_int("iteration", r);
        }
        json_obj_end();
        json_obj("in");
        json_int("patterns", N_JT);
        json_obj_end();
        json_str("why", "repeated JOYTEST write / JOYxDAT read loop: bits 7-2 of every counter must read "
                 "back the written pattern on every iteration (signature of mismatches must stay 0)");
        amt_case_close();
    }
}

/* ============================================================ POTGO / POTINP */
static UWORD pot_set_read(UWORD potgo)
{
    CWR(R_POTGO, potgo);
    wait_lines(16);     /* ~1 ms: the pins carry the pot capacitors */
    return CRD(R_POTINP);
}

static const UWORD pins[4] = { P_LX, P_LY, P_RX, P_RY };

static void potinp_tests(void)
{
    UWORD potgo[16], got[16], mask[16];
    int n = 0, i, p, d, bad = -1;

    /* single pin as output, the other three inputs: data 1, then 0 */
    for (p = 0; p < 4; p++)
        for (d = 1; d >= 0; d--) {
            potgo[n] = (UWORD)((pins[p] << 1) | (d ? pins[p] : 0));
            mask[n] = pins[p];
            n++;
        }
    for (i = 0; i < n; i++) {
        got[i] = pot_set_read(potgo[i]);
        if (bad < 0 && ((got[i] ^ potgo[i]) & mask[i]))
            bad = i;
    }
    CWR(R_POTGO, POTGO_IDLE);
    if (bad < 0)
        amt_case("potinp_out_readback_single", AMT_ST_PASS, 0);
    else {
        amt_case_open("potinp_out_readback_single", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("potinp_and_mask", potgo[bad] & mask[bad], 4);
        json_obj_end();
        json_obj("got");
        json_hexn("potinp", got[bad], 4);
        json_obj_end();
        json_obj("in");
        json_hexn("potgo", potgo[bad], 4);
        json_hexn("mask", mask[bad], 4);
        json_obj_end();
        json_str("why", "HRM A POTGO/POTINP: a pot pin with its OUTxx bit set is an output driving DATxx; "
                 "POTINP must read that level back (nothing pressed on the port)");
        amt_case_close();
    }

    /* several outputs at once: pin 9s in all four combinations with both
     * pin 5s high; then pin 5s in all combinations with the pin 9s as
     * inputs (a pin 5 driven low selects CD32 pad shift mode, in which the
     * pad drives pin 9 itself, so pin 9 is never an output then) */
    n = 0;
    bad = -1;
    for (i = 0; i < 4; i++) {
        potgo[n] = (UWORD)(0xaa00 | P_LX | P_RX | ((i & 1) ? P_LY : 0) | ((i & 2) ? P_RY : 0));
        mask[n] = P_LX | P_LY | P_RX | P_RY;
        n++;
    }
    for (i = 0; i < 4; i++) {
        potgo[n] = (UWORD)(((P_LX | P_RX) << 1) | ((i & 1) ? P_LX : 0) | ((i & 2) ? P_RX : 0));
        mask[n] = P_LX | P_RX;
        n++;
    }
    for (i = 0; i < n; i++) {
        got[i] = pot_set_read(potgo[i]);
        if (bad < 0 && ((got[i] ^ potgo[i]) & mask[i]))
            bad = i;
    }
    CWR(R_POTGO, POTGO_IDLE);
    if (bad < 0)
        amt_case("potinp_out_readback_multi", AMT_ST_PASS, 0);
    else {
        amt_case_open("potinp_out_readback_multi", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("potinp_and_mask", potgo[bad] & mask[bad], 4);
        json_obj_end();
        json_obj("got");
        json_hexn("potinp", got[bad], 4);
        json_obj_end();
        json_obj("in");
        json_hexn("potgo", potgo[bad], 4);
        json_hexn("mask", mask[bad], 4);
        json_obj_end();
        json_str("why", "HRM A POTGO/POTINP: every pot pin set as output (OUTxx=1) reads back its DATxx "
                 "in POTINP, independently of the other pins");
        amt_case_close();
    }
}

static void pot_counter_test(void)
{
    static const UWORD at_lines[5] = { 0, 2, 10, 313, 626 };
    UWORD p0[5], p1[5];
    ULONG t0;
    int i;
    CWR(R_POTGO, 0x0001);   /* all four pins input, START */
    t0 = cia_now();
    for (i = 0; i < 5; i++) {
        while (cia_now() - t0 < at_lines[i] * 227UL)
            ;
        p0[i] = CRD(R_POT0DAT);
        p1[i] = CRD(R_POT1DAT);
    }
    CWR(R_POTGO, POTGO_IDLE);
    amt_case_open("pot_counters_start", AMT_ST_PASS);
    json_obj("timing");
    json_arr("lines");
    for (i = 0; i < 5; i++)
        json_int(0, at_lines[i]);
    json_arr_end();
    json_arr("pot0dat");
    for (i = 0; i < 5; i++)
        json_hexn(0, p0[i], 4);
    json_arr_end();
    json_arr("pot1dat");
    for (i = 0; i < 5; i++)
        json_hexn(0, p1[i], 4);
    json_arr_end();
    json_obj_end();
    json_obj("in");
    json_hexn("potgo", 0x0001, 4);
    json_obj_end();
    amt_case_close();
}

/* ============================================================ keyboard */
static UBYTE kbd_enc(UBYTE code)   /* what SDR holds for a code (bit 7 = up) */
{
    return (UBYTE)~((code << 1) | (code >> 7));
}

static UBYTE kbd_dec(UBYTE sdr)
{
    UBYTE v = (UBYTE)~sdr;
    return (UBYTE)((v >> 1) | (v << 7));
}

/* wait up to `frames` for a keyboard byte (ICR SP, serial port in input
 * mode); the byte is read but not yet acknowledged */
static int kbd_wait(UBYTE *sdr, ULONG frames)
{
    ULONG f0 = amt_frames_now();
    for (;;) {
        if (cia_rd(CIAA_BASE, C_ICR) & ICR_SP) {
            *sdr = cia_rd(CIAA_BASE, C_SDR);
            return 1;
        }
        if (amt_frames_now() - f0 > frames)
            return 0;
    }
}

static ULONG kbd_strays;

/* acknowledge whatever the keyboard may be waiting for, then drain */
static void kbd_flush(void)
{
    UBYTE b;
    int k;
    cia_kbd_handshake();
    for (k = 0; k < 16 && kbd_wait(&b, 6); k++) {
        cia_kbd_handshake();
        kbd_strays++;
    }
}

/* one key event: request, wait for the byte, acknowledge.
 * returns the request result (-1/0) when not acked, 2 when acked but no
 * byte came within 1 s, 1 with the raw SDR value in *b */
static int kbd_event(UBYTE code, int up, UBYTE *b, int ack)
{
    int r;
    a_begin();
    a_int("code", code);
    a_int("up", up);
    r = req(H_KEY, up ? "up" : "down", a_end());
    if (r <= 0)
        return r;
    if (!kbd_wait(b, 50))
        return 2;
    if (ack)
        cia_kbd_handshake();
    return 1;
}

static void kbd_fail_seq(const char *id, const UBYTE *exp, const UBYTE *got, const UBYTE *have,
                         int n, const char *why, const char *what)
{
    int i;
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_arr("sdr");
    for (i = 0; i < n; i++)
        json_hexn(0, kbd_enc(exp[i]), 2);
    json_arr_end();
    json_arr("code");
    for (i = 0; i < n; i++)
        json_hexn(0, exp[i], 2);
    json_arr_end();
    json_obj_end();
    json_obj("got");
    json_arr("sdr");
    for (i = 0; i < n; i++)
        if (have[i])
            json_hexn(0, got[i], 2);
        else
            json_str(0, "none");
    json_arr_end();
    json_arr("code");
    for (i = 0; i < n; i++)
        if (have[i])
            json_hexn(0, kbd_dec(got[i]), 2);
        else
            json_str(0, "none");
    json_arr_end();
    json_obj_end();
    json_obj("in");
    json_str("events", what);
    json_obj_end();
    json_str("why", why);
    amt_case_close();
}

/* a sequence of key events, each acknowledged; 1 = all acked (result in
 * got/have), else the req result for host_untestable */
static int kbd_seq(const UBYTE *codes, int n, UBYTE *got, UBYTE *have)
{
    int i, r;
    for (i = 0; i < n; i++) {
        r = kbd_event(codes[i] & 0x7f, codes[i] >> 7, &got[i], 1);
        if (r <= 0)
            return r;
        have[i] = r == 1;
    }
    return 1;
}

static int seq_ok(const UBYTE *exp, const UBYTE *got, const UBYTE *have, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!have[i] || got[i] != kbd_enc(exp[i]))
            return 0;
    return 1;
}

static const char kbd_why_enc[] =
    "HRM ch. 8 keyboard: the keyboard sends the code rotated left one bit (bits 6..0 then the up "
    "flag bit 7) and inverted, so CIA-A SDR = ~((code << 1) | (code >> 7)); one byte per key down "
    "and one (bit 7 set) per key up";

static void kbd_down_up_test(void)
{
    static const UBYTE seq[2] = { 0x20, 0xa0 };   /* A down, A up */
    UBYTE got[2], have[2] = { 0, 0 };
    int r = kbd_seq(seq, 2, got, have);
    if (r <= 0) {
        host_untestable("kbd_key_down_up", H_KEY, r);
        return;
    }
    if (seq_ok(seq, got, have, 2)) {
        amt_case_open("kbd_key_down_up", AMT_ST_PASS);
        json_obj("in");
        json_str("events", "20 down, 20 up");
        json_obj_end();
        amt_case_close();
    } else
        kbd_fail_seq("kbd_key_down_up", seq, got, have, 2, kbd_why_enc, "20 down, 20 up");
}

/* representative set: every row, numeric pad, cursor, function and
 * modifier keys. Not: caps lock ($62, toggling LED key), $68-$7F. */
static const UBYTE kset[] = {
    0x00, 0x01, 0x05, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0f,
    0x10, 0x14, 0x19, 0x1a, 0x1b, 0x1d, 0x1e, 0x1f,
    0x20, 0x24, 0x28, 0x2a, 0x2b, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x35, 0x39, 0x3a, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x4a, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x54, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x61, 0x63, 0x64, 0x65, 0x66, 0x67,
};
#define N_KSET (sizeof(kset) / sizeof(kset[0]))

static void kbd_set_test(void)
{
    UBYTE gd[N_KSET], gu[N_KSET], hd[N_KSET], hu[N_KSET], x;
    int i, n = 0, r = 1, nobyte = 0, step = AMT_QUICK() ? 4 : 1, bad = -1, tried = 0;
    ULONG extra = 0;
    UBYTE extra_b[4];
    UBYTE extra_after[4];

    for (i = 0; i < (int)N_KSET; i += step) {
        UBYTE c = kset[i];
        hd[n] = hu[n] = 0;
        r = kbd_event(c, 0, &gd[n], 1);
        if (r <= 0)
            break;
        hd[n] = r == 1;
        r = kbd_event(c, 1, &gu[n], 1);
        if (r <= 0)
            break;
        hu[n] = r == 1;
        /* no further byte for this key */
        while (kbd_wait(&x, 3)) {
            cia_kbd_handshake();
            if (extra < 4) {
                extra_b[extra] = x;
                extra_after[extra] = c;
            }
            extra++;
        }
        nobyte = (!hd[n] && !hu[n]) ? nobyte + 1 : 0;
        if (bad < 0 && (!hd[n] || !hu[n] || gd[n] != kbd_enc(c) || gu[n] != kbd_enc(c | 0x80)))
            bad = n;
        n++;
        tried = i;
        if (nobyte >= 3)
            break;   /* the keyboard is not delivering: stop, report */
    }
    if (r <= 0 && n == 0) {
        host_untestable("kbd_keycode_set", H_KEY, r);
        host_untestable("kbd_no_spurious_bytes", H_KEY, -1);
        return;
    }
    if (r <= 0) {
        /* the host stopped acknowledging part-way */
        amt_case_open("kbd_keycode_set", AMT_ST_UNTESTABLE);
        json_str("why", hwhy[H_KEY]);
        json_obj("ctx");
        json_int("keys_done", n);
        json_hexn("stopped_at", kset[i], 2);
        json_obj_end();
        amt_case_close();
    } else if (bad < 0 && nobyte < 3) {
        amt_case_open("kbd_keycode_set", AMT_ST_PASS);
        json_obj("in");
        json_int("keys", n);
        json_int("step", step);
        json_obj_end();
        amt_case_close();
    } else {
        int k;
        amt_case_open("kbd_keycode_set", AMT_ST_FAIL);
        json_obj("expect");
        json_str("each", "down: SDR = ~(code<<1), up: SDR = ~((code<<1)|1)");
        json_obj_end();
        json_obj("got");
        json_arr("bad");     /* code, sdr down, sdr up ("none" = no byte) */
        for (k = 0; k < n; k++) {
            UBYTE c = kset[k * step];
            if (hd[k] && hu[k] && gd[k] == kbd_enc(c) && gu[k] == kbd_enc(c | 0x80))
                continue;
            json_arr(0);
            json_hexn(0, c, 2);
            if (hd[k])
                json_hexn(0, gd[k], 2);
            else
                json_str(0, "none");
            if (hu[k])
                json_hexn(0, gu[k], 2);
            else
                json_str(0, "none");
            json_arr_end();
        }
        json_arr_end();
        if (nobyte >= 3)
            json_str("stopped", "3 keys in a row gave no byte at all");
        json_obj_end();
        json_obj("in");
        json_int("keys", n);
        json_int("step", step);
        json_hexn("last_key", kset[tried], 2);
        json_obj_end();
        json_str("why", kbd_why_enc);
        amt_case_close();
    }

    if (r <= 0) {
        host_untestable("kbd_no_spurious_bytes", H_KEY, -1);
        return;
    }
    if (!extra)
        amt_case("kbd_no_spurious_bytes", AMT_ST_PASS, 0);
    else {
        ULONG k;
        amt_case_open("kbd_no_spurious_bytes", AMT_ST_FAIL);
        json_obj("expect");
        json_int("extra_bytes", 0);
        json_obj_end();
        json_obj("got");
        json_int("extra_bytes", (long)extra);
        json_arr("first");      /* [after key, sdr, decoded] */
        for (k = 0; k < extra && k < 4; k++) {
            json_arr(0);
            json_hexn(0, extra_after[k], 2);
            json_hexn(0, extra_b[k], 2);
            json_hexn(0, kbd_dec(extra_b[k]), 2);
            json_arr_end();
        }
        json_arr_end();
        json_obj_end();
        json_obj("in");
        json_int("keys", n);
        json_int("wait_frames", 3);
        json_obj_end();
        json_str("why", "HRM ch. 8 keyboard: one key press gives exactly one down and one up code; "
                 "the Amiga keyboard has no typematic repeat (repeat is software)");
        amt_case_close();
    }
}

static void kbd_rollover_test(void)
{
    static const UBYTE seq[4] = { 0x20, 0x21, 0xa1, 0xa0 };   /* A dn, S dn, S up, A up */
    UBYTE got[4], have[4] = { 0, 0, 0, 0 };
    int r = kbd_seq(seq, 4, got, have);
    if (r <= 0) {
        host_untestable("kbd_rollover_order", H_KEY, r);
        return;
    }
    if (seq_ok(seq, got, have, 4))
        amt_case("kbd_rollover_order", AMT_ST_PASS, 0);
    else
        kbd_fail_seq("kbd_rollover_order", seq, got, have, 4,
                     "HRM ch. 8 keyboard: with two keys held, each transition is sent in order as its "
                     "own down/up code", "20 down, 21 down, 21 up, 20 up");
}

/* the keyboard must not send the next code before the handshake */
static void kbd_hold_test(void)
{
    UBYTE b1 = 0, b2 = 0, acc = 0, sdr_mid;
    int r, early = 0;
    ULONG t0;
    t0 = cia_now();                     /* before even the first host round trip */
    r = kbd_event(0x35, 0, &b1, 0);       /* B down, not acknowledged yet */
    if (r <= 0) {
        host_untestable("kbd_hold_until_handshake", H_KEY, r);
        return;
    }
    if (r == 2) {
        amt_case_open("kbd_hold_until_handshake", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("sdr_first", kbd_enc(0x35), 2);
        json_obj_end();
        json_obj("got");
        json_str("sdr_first", "none");
        json_obj_end();
        json_str("why", "key down acknowledged by the host but no keyboard byte (ICR SP) within 1 s");
        amt_case_close();
        return;
    }
    a_begin();
    a_int("code", 0x35);
    a_int("up", 1);
    r = req(H_KEY, "up", a_end());
    if (r <= 0) {
        cia_kbd_handshake();
        host_untestable("kbd_hold_until_handshake", H_KEY, r);
        return;
    }
    /* Include the host round trip in the original byte's deadline. */
    if (!hw_keyboard_window(t0, cia_now())) {
        cia_kbd_handshake();
        amt_case("kbd_hold_until_handshake", AMT_ST_UNTESTABLE,
                 "host round trip left insufficient coverage before the keyboard handshake deadline");
        return;
    }
    t0 = cia_now();
    while (cia_now() - t0 < 30UL * 3547UL)
        acc |= cia_rd(CIAA_BASE, C_ICR);
    sdr_mid = cia_rd(CIAA_BASE, C_SDR);
    if (acc & ICR_SP)
        early = 1;
    cia_kbd_handshake();
    r = kbd_wait(&b2, 50);
    if (r)
        cia_kbd_handshake();
    if (!early && sdr_mid == b1 && b1 == kbd_enc(0x35) && r && b2 == kbd_enc(0xb5))
        amt_case("kbd_hold_until_handshake", AMT_ST_PASS, 0);
    else {
        amt_case_open("kbd_hold_until_handshake", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("sdr_first", kbd_enc(0x35), 2);
        json_int("sp_before_handshake", 0);
        json_hexn("sdr_before_handshake", kbd_enc(0x35), 2);
        json_hexn("sdr_after_handshake", kbd_enc(0xb5), 2);
        json_obj_end();
        json_obj("got");
        json_hexn("sdr_first", b1, 2);
        json_int("sp_before_handshake", early);
        json_hexn("sdr_before_handshake", sdr_mid, 2);
        if (r)
            json_hexn("sdr_after_handshake", b2, 2);
        else
            json_str("sdr_after_handshake", "none");
        json_obj_end();
        json_obj("in");
        json_str("events", "35 down (no handshake), 35 up requested, 30 ms, handshake");
        json_obj_end();
        json_str("why", "HRM ch. 8 keyboard: after each byte the keyboard waits for the handshake "
                 "(up to 143 ms) before it sends the next code; the queued up code follows it");
        amt_case_close();
    }
}

static void keyboard_tests(void)
{
    kbd_flush();
    kbd_down_up_test();
    kbd_set_test();
    kbd_rollover_test();
    kbd_hold_test();
    kbd_flush();
    if (kbd_strays) {
        char m[48], *d = m;
        const char *s = "input: stray keyboard bytes acknowledged: ";
        while (*s)
            *d++ = *s++;
        d = amt_utoa(d, kbd_strays);
        amt_log(m);
    }
}

/* ============================================================ mouse */
static int mouse_req(int dx, int dy, int buttons)
{
    a_begin();
    a_int("dx", dx);
    a_int("dy", dy);
    a_int("buttons", buttons);
    a_int("port", 0);
    return req(H_MOUSE, "mouse", a_end());
}

static void mouse_move_case(const char *id, int dx, int dy, int preset, UWORD preset_v)
{
    UWORD before, v, last;
    ULONG f0, fchg, now, fmatch = 0;
    int gx = 0, gy = 0, r, match = 0, moved = 0;

    if (preset)
        CWR(R_JOYTEST, preset_v);
    before = CRD(R_JOY0DAT);
    r = mouse_req(dx, dy, 0);
    if (r <= 0) {
        host_untestable(id, H_MOUSE, r);
        return;
    }
    f0 = fchg = amt_frames_now();
    last = before;
    for (;;) {
        v = CRD(R_JOY0DAT);
        now = amt_frames_now();
        if (v != last) {
            last = v;
            fchg = now;
            moved = 1;
        }
        gx = (BYTE)((v & 0xff) - (before & 0xff));
        gy = (BYTE)((v >> 8) - (before >> 8));
        if (gx == dx && gy == dy) {
            if (!match)
                fmatch = now - f0;
            match = 1;
        } else
            match = 0;
        if (match && now - fchg >= 5)
            break;
        if (now - fchg >= 25)
            break;         /* stable but not matching (or no movement at all) */
        if (now - f0 >= 150)
            break;
    }
    amt_case_open(id, match ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!match) {
        json_obj("expect");
        json_int("dx", dx);
        json_int("dy", dy);
        json_obj_end();
        json_obj("got");
        json_int("dx", gx);
        json_int("dy", gy);
        json_hexn("joy0dat_before", before, 4);
        json_hexn("joy0dat_after", last, 4);
        json_int("moved", moved);
        json_obj_end();
        json_str("why", "HRM ch. 8 mouse: JOY0DAT X (bits 7-0) counts +1 per quadrature step to the "
                 "right, Y (bits 15-8) +1 per step down (toward the user), each an independent 8-bit "
                 "counter (wraps mod 256, no carry into the other); the host moved the mouse by dx/dy counts");
    }
    json_obj("in");
    json_int("dx", dx);
    json_int("dy", dy);
    if (preset)
        json_hexn("joytest", preset_v, 4);
    json_obj_end();
    json_obj("timing");
    json_int("frames_to_match", match ? (long)fmatch : -1);
    json_obj_end();
    amt_case_close();
}

/* read a button line: 0 = pressed (low), 1 = released */
#define B_LEFT   0
#define B_RIGHT  1
#define B_MIDDLE 2
#define B_RIGHT_IN 3

static int mouse_btn_level(int which)
{
    switch (which) {
    case B_LEFT:
        return (cia_rd(CIAA_BASE, C_PRA) & FIR0) ? 1 : 0;
    case B_RIGHT:
        return (CRD(R_POTINP) & P_LY) ? 1 : 0;
    case B_MIDDLE:
        return (CRD(R_POTINP) & P_LX) ? 1 : 0;
    default:
        return (CRD(R_POTINP) & P_LY) ? 1 : 0;
    }
}

/* wait up to 1 s for the level, then require it for 2 more frames */
static int wait_level(int (*rd)(int), int which, int want, ULONG *frames)
{
    ULONG f0 = amt_frames_now(), fok = 0;
    int ok = 0;
    for (;;) {
        int l = rd(which);
        ULONG now = amt_frames_now();
        if (l == want) {
            if (!ok)
                fok = now;
            ok = 1;
            if (hw_stable_complete(now, fok, ok))
                break;
        } else
            ok = 0;
        if (now - f0 >= 50) {
            ok = 0;
            break;
        }
    }
    if (frames)
        *frames = fok - f0;
    return ok ? want : !want;
}

static void mouse_button_case(const char *id, int which, int mask, const char *why)
{
    UWORD j0, j1, potgo = which == B_RIGHT_IN ? 0x0000 : POTGO_IDLE;
    int r, rel0, prs, rel1, ok;
    ULONG fp = 0;
    CWR(R_POTGO, potgo);
    wait_lines(16);
    r = mouse_req(0, 0, 0);
    if (r <= 0) {
        CWR(R_POTGO, POTGO_IDLE);
        host_untestable(id, H_MOUSE, r);
        return;
    }
    rel0 = wait_level(mouse_btn_level, which, 1, 0);
    j0 = CRD(R_JOY0DAT);
    r = mouse_req(0, 0, mask);
    if (r <= 0) {
        CWR(R_POTGO, POTGO_IDLE);
        host_untestable(id, H_MOUSE, r);
        return;
    }
    prs = wait_level(mouse_btn_level, which, 0, &fp);
    r = mouse_req(0, 0, 0);
    if (r <= 0) {
        CWR(R_POTGO, POTGO_IDLE);
        host_untestable(id, H_MOUSE, r);
        return;
    }
    rel1 = wait_level(mouse_btn_level, which, 1, 0);
    j1 = CRD(R_JOY0DAT);
    CWR(R_POTGO, POTGO_IDLE);
    /* input mode: a released line floats (pull-ups are a property of the
     * mouse), so only the pressed level is checked there */
    if (which == B_RIGHT_IN)
        ok = prs == 0 && j0 == j1;
    else
        ok = rel0 == 1 && prs == 0 && rel1 == 1 && j0 == j1;
    amt_case_open(id, ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        if (which != B_RIGHT_IN)
            json_int("released", 1);
        json_int("pressed", 0);
        if (which != B_RIGHT_IN)
            json_int("released_again", 1);
        json_str("joy0dat", "unchanged");
        json_obj_end();
        json_obj("got");
        json_int("released", rel0);
        json_int("pressed", prs);
        json_int("released_again", rel1);
        json_hexn("joy0dat_pressed", j0, 4);
        json_hexn("joy0dat_after", j1, 4);
        json_obj_end();
        json_str("why", why);
    }
    json_obj("in");
    json_int("buttons", mask);
    json_hexn("potgo", potgo, 4);
    json_obj_end();
    json_obj("timing");
    json_int("frames_to_press", (long)fp);
    json_obj_end();
    amt_case_close();
}

static void mouse_tests(void)
{
    int big = AMT_QUICK() ? 60 : 100;
    mouse_move_case("mouse_move_right", 10, 0, 0, 0);
    mouse_move_case("mouse_move_left", -10, 0, 0, 0);
    mouse_move_case("mouse_move_down", 0, 10, 0, 0);
    mouse_move_case("mouse_move_up", 0, -10, 0, 0);
    mouse_move_case("mouse_move_diagonal", 3, -5, 0, 0);
    mouse_move_case("mouse_move_large", big, -big, 0, 0);
    /* counters preset with JOYTEST next to the 8-bit wrap */
    mouse_move_case("mouse_wrap_x", 8, 0, 1, 0x80fc);
    mouse_move_case("mouse_wrap_y", 0, -8, 1, 0x0080);
    mouse_button_case("mouse_left_button", B_LEFT, 1,
                      "HRM ch. 8: the left mouse button is port 0 pin 6 = CIA-A PRA bit 6 /FIR0, low "
                      "while pressed; buttons do not move the counters");
    mouse_button_case("mouse_right_button", B_RIGHT, 2,
                      "HRM ch. 8 / A POTGO: the right button is port 0 pin 9; with POTGO OUTLY=1 "
                      "DATLY=1 (POTGO=$FF00, the OS setting) POTINP bit 10 DATLY reads 1, and 0 while "
                      "the button pulls the line low");
    mouse_button_case("mouse_right_button_input", B_RIGHT_IN, 2,
                      "HRM A POTGO: with OUTLY=0 (pin 9 input) the pressed right button pulls the line "
                      "low: POTINP DATLY = 0");
    mouse_button_case("mouse_middle_button", B_MIDDLE, 4,
                      "HRM ch. 8: the middle button is port 0 pin 5; with POTGO OUTLX=1 DATLX=1 POTINP "
                      "bit 8 DATLX reads 1, and 0 while pressed");
}

/* ============================================================ joystick */
static int joy_req(const char *dirs, int fire)
{
    a_begin();
    a_int("port", 1);
    a_str("dirs", dirs);
    a_int("fire", fire);
    return req(H_JOY, "joy", a_end());
}

/* HRM ch. 8: right = X1, left = Y1, down = X1^X0, up = Y1^Y0 */
static UBYTE joy_decode(UWORD d)
{
    UBYTE r = (d >> 1) & 1, l = (d >> 9) & 1;
    UBYTE dn = ((d >> 1) ^ d) & 1, up = ((d >> 9) ^ (d >> 8)) & 1;
    return (UBYTE)(up << 3 | dn << 2 | l << 1 | r);   /* bit0 r, 1 l, 2 d, 3 u */
}

static UBYTE dirs_mask(const char *s)
{
    UBYTE m = 0;
    for (; *s; s++)
        m |= *s == 'r' ? 1 : *s == 'l' ? 2 : *s == 'd' ? 4 : *s == 'u' ? 8 : 0;
    return m;
}

static int joy_dir_level(int unused)
{
    (void)unused;
    return joy_decode(CRD(R_JOY1DAT));
}

static void joy_tests(void)
{
    static const char *const pos[9] = { "", "u", "d", "l", "r", "ul", "ur", "dl", "dr" };
    UBYTE got[9];
    UWORD raw[9];
    int i, r = 1, bad = -1;
    ULONG f;

    for (i = 0; i < 9; i++) {
        r = joy_req(pos[i], 0);
        if (r <= 0)
            break;
        got[i] = (UBYTE)wait_level(joy_dir_level, 0, dirs_mask(pos[i]), &f);
        /* wait_level returns want or !want; re-read the real value */
        raw[i] = CRD(R_JOY1DAT);
        got[i] = joy_decode(raw[i]);
        if (bad < 0 && got[i] != dirs_mask(pos[i]))
            bad = i;
    }
    if (r <= 0)
        host_untestable("joy_directions", H_JOY, r);
    else if (bad < 0)
        amt_case("joy_directions", AMT_ST_PASS, 0);
    else {
        amt_case_open("joy_directions", AMT_ST_FAIL);
        json_obj("expect");
        json_str("dirs", pos[bad]);
        json_hexn("decoded_urdl", dirs_mask(pos[bad]), 1);
        json_obj_end();
        json_obj("got");
        json_hexn("decoded_urdl", got[bad], 1);
        json_hexn("joy1dat", raw[bad], 4);
        json_obj_end();
        json_obj("in");
        json_int("port", 1);
        json_str("bits", "decoded: bit0 right=X1, bit1 left=Y1, bit2 down=X1^X0, bit3 up=Y1^Y0");
        json_obj_end();
        json_str("why", "HRM ch. 8 joystick: JOY1DAT right = bit 1, left = bit 9, down = bit 1 XOR bit 0, "
                 "up = bit 9 XOR bit 8");
        amt_case_close();
    }

    /* fire 1: CIA-A PRA bit 7 /FIR1 */
    {
        int rel0, prs, rel1;
        if ((r = joy_req("", 0)) <= 0) {
            host_untestable("joy_fire", H_JOY, r);
        } else {
            rel0 = (cia_rd(CIAA_BASE, C_PRA) & FIR1) ? 1 : 0;
            if ((r = joy_req("", 1)) <= 0)
                host_untestable("joy_fire", H_JOY, r);
            else {
                ULONG f0 = amt_frames_now();
                while (((prs = (cia_rd(CIAA_BASE, C_PRA) & FIR1) ? 1 : 0)) && amt_frames_now() - f0 < 50)
                    ;
                if ((r = joy_req("", 0)) <= 0)
                    host_untestable("joy_fire", H_JOY, r);
                else {
                    f0 = amt_frames_now();
                    while (!((rel1 = (cia_rd(CIAA_BASE, C_PRA) & FIR1) ? 1 : 0)) && amt_frames_now() - f0 < 50)
                        ;
                    if (rel0 == 1 && prs == 0 && rel1 == 1)
                        amt_case("joy_fire", AMT_ST_PASS, 0);
                    else {
                        amt_case_open("joy_fire", AMT_ST_FAIL);
                        json_obj("expect");
                        json_str("pra_bit7", "1,0,1");
                        json_obj_end();
                        json_obj("got");
                        json_int("released", rel0);
                        json_int("pressed", prs);
                        json_int("released_again", rel1);
                        json_obj_end();
                        json_str("why", "HRM ch. 8: port 1 fire is pin 6 = CIA-A PRA bit 7 /FIR1, low while pressed");
                        amt_case_close();
                    }
                }
            }
        }
    }

    /* fire 2: port 1 pin 9, POTINP bit 14 DATRY with POTGO=$FF00 */
    {
        int rel0, prs = 1, rel1 = 0;
        ULONG f0;
        CWR(R_POTGO, POTGO_IDLE);
        if ((r = joy_req("", 0)) <= 0)
            host_untestable("joy_fire2", H_JOY, r);
        else {
            wait_lines(16);
            rel0 = (CRD(R_POTINP) & P_RY) ? 1 : 0;
            if ((r = joy_req("", 2)) <= 0)
                host_untestable("joy_fire2", H_JOY, r);
            else {
                f0 = amt_frames_now();
                while ((prs = (CRD(R_POTINP) & P_RY) ? 1 : 0) && amt_frames_now() - f0 < 50)
                    ;
                if ((r = joy_req("", 0)) <= 0)
                    host_untestable("joy_fire2", H_JOY, r);
                else {
                    f0 = amt_frames_now();
                    while (!(rel1 = (CRD(R_POTINP) & P_RY) ? 1 : 0) && amt_frames_now() - f0 < 50)
                        ;
                    if (rel0 == 1 && prs == 0 && rel1 == 1)
                        amt_case("joy_fire2", AMT_ST_PASS, 0);
                    else {
                        amt_case_open("joy_fire2", AMT_ST_FAIL);
                        json_obj("expect");
                        json_str("potinp_bit14", "1,0,1");
                        json_obj_end();
                        json_obj("got");
                        json_int("released", rel0);
                        json_int("pressed", prs);
                        json_int("released_again", rel1);
                        json_obj_end();
                        json_obj("in");
                        json_hexn("potgo", POTGO_IDLE, 4);
                        json_obj_end();
                        json_str("why", "HRM ch. 8 / A POTGO: the second fire button is port 1 pin 9; with "
                                 "OUTRY=1 DATRY=1 POTINP bit 14 reads 1, 0 while pressed");
                        amt_case_close();
                    }
                }
            }
        }
    }
}

/* ============================================================ CD32 pad */
/* lowlevel.library protocol on port 1: /FIR1 output (clock), pin 5 low
 * (shift mode), 9 reads of pin 9 (POTINP DATRY), one clock pulse after
 * each. Result bit i = level of read i: 0..6 = blue, red, yellow, green,
 * forward, reverse, play (0 = pressed), 7 = 1, 8 = 0 for a CD32 pad. */
static UWORD cd32_read(void)
{
    UBYTE ddra = cia_rd(CIAA_BASE, C_DDRA);
    UBYTE pra = cia_rd(CIAA_BASE, C_PRA) & 0x02;   /* LED kept, OVL = 0 */
    UWORD v = 0;
    int i;
    cia_wr(CIAA_BASE, C_PRA, pra);                 /* clock latch low */
    cia_wr(CIAA_BASE, C_DDRA, ddra | FIR1);        /* /FIR1 output */
    CWR(R_POTGO, 0x2f00);   /* RX output low (shift mode), RY input; port 0 high */
    wait_lines(3);
    for (i = 0; i < 9; i++) {
        if (CRD(R_POTINP) & P_RY)
            v |= 1 << i;
        cia_wr(CIAA_BASE, C_PRA, pra | FIR1);
        cia_delay(4);
        cia_wr(CIAA_BASE, C_PRA, pra);
        cia_delay(4);
    }
    CWR(R_POTGO, POTGO_IDLE);                      /* pin 5 high: normal mode */
    cia_wr(CIAA_BASE, C_DDRA, ddra);
    wait_lines(3);
    return v;
}

static const char *const cd32_names[7] = { "blue", "red", "yellow", "green", "forward", "reverse", "play" };

static int cd32_req(int mask)
{
    a_begin();
    a_int("port", 1);
    a_int("buttons", mask);
    return req(H_CD32, "cd32pad", a_end());
}

/* read until the expected value or 1 s, then confirm with 2 more reads */
static UWORD cd32_wait(UWORD want)
{
    ULONG f0 = amt_frames_now();
    UWORD v;
    int k;
    for (;;) {
        v = cd32_read();
        if (v == want) {
            for (k = 0; k < 2 && v == want; k++) {
                wait_frames(1);
                v = cd32_read();
            }
            return v;
        }
        if (amt_frames_now() - f0 >= 50)
            return v;
        wait_frames(1);
    }
}

static void cd32_fail(const char *id, int mask, UWORD want, UWORD got, const char *why)
{
    int i;
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_hexn("reads", want, 3);
    json_obj_end();
    json_obj("got");
    json_hexn("reads", got, 3);
    json_arr("pressed");
    for (i = 0; i < 7; i++)
        if (!(got & (1 << i)))
            json_str(0, cd32_names[i]);
    json_arr_end();
    json_int("id_bit7", (got >> 7) & 1);
    json_int("id_bit8", (got >> 8) & 1);
    json_obj_end();
    json_obj("in");
    json_hexn("buttons", mask, 2);
    json_str("reads_bits", "bit i = POTINP DATRY at read i: 0 blue 1 red 2 yellow 3 green 4 forward "
             "5 reverse 6 play (0 = pressed), 7 id (1), 8 id (0)");
    json_obj_end();
    json_str("why", why);
    amt_case_close();
}

static const char cd32_why[] =
    "CD32 pad (lowlevel.library protocol): with pin 5 driven low the pad shifts its buttons out on "
    "pin 9, one per /FIR1 clock: blue, red, yellow, green, forward, reverse, play (low = pressed), "
    "then 1, then 0 (the pad id)";

static void cd32_tests(void)
{
    UWORD v, want;
    int r, i, bad = -1;
    UWORD gotb[7];
    static const UBYTE combos[3] = { 0x7f, 0x55, 0x2a };
    UWORD gotc[3];

    r = cd32_req(0);
    if (r <= 0) {
        host_untestable("cd32_id_released", H_CD32, r);
        host_untestable("cd32_buttons_single", H_CD32, -1);
        host_untestable("cd32_buttons_combo", H_CD32, -1);
        host_untestable("cd32_normal_mode", H_CD32, -1);
        return;
    }
    v = cd32_wait(0x0ff);
    if (v == 0x0ff)
        amt_case("cd32_id_released", AMT_ST_PASS, 0);
    else
        cd32_fail("cd32_id_released", 0, 0x0ff, v, cd32_why);

    for (i = 0; i < 7; i++) {
        r = cd32_req(1 << i);
        if (r <= 0)
            break;
        want = (UWORD)(0x0ff & ~(1 << i));
        gotb[i] = cd32_wait(want);
        if (bad < 0 && gotb[i] != want)
            bad = i;
    }
    if (r <= 0)
        host_untestable("cd32_buttons_single", H_CD32, r);
    else if (bad < 0)
        amt_case("cd32_buttons_single", AMT_ST_PASS, 0);
    else
        cd32_fail("cd32_buttons_single", 1 << bad, (UWORD)(0x0ff & ~(1 << bad)), gotb[bad], cd32_why);

    bad = -1;
    r = 1;
    for (i = 0; i < 3 && r > 0; i++) {
        r = cd32_req(combos[i]);
        if (r <= 0)
            break;
        gotc[i] = cd32_wait((UWORD)(0x0ff & ~combos[i]));
        if (bad < 0 && gotc[i] != (UWORD)(0x0ff & ~combos[i]))
            bad = i;
    }
    if (r <= 0)
        host_untestable("cd32_buttons_combo", H_CD32, r);
    else if (bad < 0)
        amt_case("cd32_buttons_combo", AMT_ST_PASS, 0);
    else
        cd32_fail("cd32_buttons_combo", combos[bad], (UWORD)(0x0ff & ~combos[bad]), gotc[bad], cd32_why);

    /* normal (2-button) mode: pin 5 high, red = /FIR1, blue = pin 9 */
    r = cd32_req(0x03);
    if (r <= 0)
        host_untestable("cd32_normal_mode", H_CD32, r);
    else {
        int red, blue, red_rel, blue_rel;
        ULONG f0 = amt_frames_now();
        CWR(R_POTGO, POTGO_IDLE);
        do {
            red = (cia_rd(CIAA_BASE, C_PRA) & FIR1) ? 1 : 0;
            blue = (CRD(R_POTINP) & P_RY) ? 1 : 0;
        } while ((red || blue) && amt_frames_now() - f0 < 50);
        r = cd32_req(0);
        if (r <= 0)
            host_untestable("cd32_normal_mode", H_CD32, r);
        else {
            f0 = amt_frames_now();
            do {
                red_rel = (cia_rd(CIAA_BASE, C_PRA) & FIR1) ? 1 : 0;
                blue_rel = (CRD(R_POTINP) & P_RY) ? 1 : 0;
            } while (!(red_rel && blue_rel) && amt_frames_now() - f0 < 50);
            if (!red && !blue && red_rel && blue_rel)
                amt_case("cd32_normal_mode", AMT_ST_PASS, 0);
            else {
                amt_case_open("cd32_normal_mode", AMT_ST_FAIL);
                json_obj("expect");
                json_int("red_pressed", 0);
                json_int("blue_pressed", 0);
                json_int("red_released", 1);
                json_int("blue_released", 1);
                json_obj_end();
                json_obj("got");
                json_int("red_pressed", red);
                json_int("blue_pressed", blue);
                json_int("red_released", red_rel);
                json_int("blue_released", blue_rel);
                json_obj_end();
                json_obj("in");
                json_hexn("buttons", 0x03, 2);
                json_hexn("potgo", POTGO_IDLE, 4);
                json_obj_end();
                json_str("why", "CD32 pad in normal mode (pin 5 high): red = fire on pin 6 (CIA-A PRA bit 7), "
                         "blue = second fire on pin 9 (POTINP bit 14), low while pressed");
                amt_case_close();
            }
        }
    }
    cd32_req(0);
}

/* ============================================================ main */
/* Does the host answer requests at all? No case of its own: when the
 * "wait" probe is not acknowledged, req() sets hnone and every host case
 * after it reports untestable without sending its request. */
static void host_probe(void)
{
    (void)req(H_WAIT, "probe", "{}");
}

static void body(void)
{
    joytest_tests();
    potinp_tests();
    pot_counter_test();
    host_probe();
    keyboard_tests();
    mouse_tests();
    joy_tests();
    cd32_tests();
}

void amt_run_input(void)
{
    int code;
    cia_save(&sav_a, CIAA_BASE, 0);
    saved_ddra = cia_rd(CIAA_BASE, C_DDRA);
    saved_joy0 = CRD(R_JOY0DAT);
    code = amt_try(body);       /* restore even after an abort */
    CWR(R_POTGO, POTGO_IDLE);
    cia_wr(CIAA_BASE, C_DDRA, saved_ddra);
    CWR(R_JOYTEST, saved_joy0);
    kbd_flush();
    cia_restore(&sav_a);
    if (code)
        amt_abort(code);
}
