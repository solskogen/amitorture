/* misc_hw part: Akiko (CD32) and CDTV hardware.
 *
 * Akiko ($B80000):
 *   $B80000.L  ID $C0CACAFE (Kickstart checks the word $CAFE at $B80002)
 *   $B80030.B  I2C data: b7 SCL, b6 SDA (reads the line state; a write drives
 *              the pins whose direction bit is 1)
 *   $B80032.B  I2C direction: b7 SCL output, b6 SDA output
 *   $B80038.L  chunky-to-planar: write 8 longwords = 32 chunky pixels (one
 *              byte each, pixel 0 in the MSB of the first longword), then
 *              read 8 longwords = bitplanes 0..7, pixel 0 in bit 31
 * The NVRAM is a 24C08 I2C EEPROM (device address 1010 A2 P1 P0). Only
 * reads are done ("random read": START, A0, word address, repeated START,
 * A1, read, NACK, STOP); a write cycle needs a data byte before STOP, which
 * is never sent, so the EEPROM is never written.
 * Akiko is looked for only on AGA machines, with a read of $B80002 inside
 * amt_try (a bus error there means "no Akiko").
 *
 * CDTV: the DMAC is an autoconfig board (Commodore West Chester, 514/3);
 * the bookmark RAM is at $DC8000 (read only, no ownership scheme exists).
 * The front panel and IR remote are reached through the CD controller's
 * command interface, not through registers, and are not tested. */
#include "misc_hw.h"

#define AKIKO      0xb80000UL
#define AK_I2C     (AKIKO + 0x30)
#define AK_I2CDIR  (AKIKO + 0x32)
#define AK_C2P     (AKIKO + 0x38)

/* ------------------------------------------------------------ C2P */
static ULONG c2p_in[8], c2p_out[8], c2p_ref[8];

static void c2p_hw(const ULONG *in, ULONG *out)
{
    int i;
    for (i = 0; i < 8; i++)
        hw_wr32(AK_C2P, in[i]);
    for (i = 0; i < 8; i++)
        out[i] = hw_rd32(AK_C2P);
}

static void c2p_cpu(const ULONG *in, ULONG *out)
{
    int p, b;
    for (b = 0; b < 8; b++)
        out[b] = 0;
    for (p = 0; p < 32; p++) {
        UBYTE px = (UBYTE)(in[p >> 2] >> (24 - 8 * (p & 3)));
        for (b = 0; b < 8; b++)
            if (px & (1 << b))
                out[b] |= 1UL << (31 - p);
    }
}

static void c2p_fill(int set)
{
    int i;
    for (i = 0; i < 8; i++) {
        ULONG v;
        switch (set) {
        case 0: v = 0; break;
        case 1: v = 0xffffffffUL; break;
        case 2: v = ((ULONG)(4 * i) << 24) | ((ULONG)(4 * i + 1) << 16) | ((ULONG)(4 * i + 2) << 8) | (4 * i + 3); break;
        case 3: v = 0x80402010UL >> (i & 3) | 0x01020408UL << (i & 3); break;
        case 4: v = (i & 1) ? 0x55aa55aaUL : 0xaa55aa55UL; break;
        default: v = amt_rand(); break;
        }
        c2p_in[i] = v;
    }
}

static ULONG c2p_hot(ULONG it)
{
    static const ULONG in[8] = { 0x01234567UL, 0x89abcdefUL, 0xfedcba98UL, 0x76543210UL,
                                 0x0f1e2d3cUL, 0x4b5a6978UL, 0x8796a5b4UL, 0xc3d2e1f0UL };
    ULONG out[8], h = 0;
    int i;
    (void)it;
    c2p_hw(in, out);
    for (i = 0; i < 8; i++)
        h = (h << 5 | h >> 27) ^ out[i];
    return h;
}

static void t_c2p(void)
{
    int set, i, bad = -1, nsets = AMT_QUICK() ? 7 : 12;
    ULONG first, badv;
    long r;
    for (set = 0; set < nsets && bad < 0; set++) {
        c2p_fill(set);
        c2p_hw(c2p_in, c2p_out);
        c2p_cpu(c2p_in, c2p_ref);
        for (i = 0; i < 8; i++)
            if (c2p_out[i] != c2p_ref[i]) {
                bad = set;
                break;
            }
    }
    if (bad < 0) {
        amt_case("akiko_c2p", AMT_ST_PASS, 0);
    } else {
        amt_case_open("akiko_c2p", AMT_ST_FAIL);
        json_obj("expect");
        json_arr("planes");
        for (i = 0; i < 8; i++)
            json_hex(0, c2p_ref[i]);
        json_arr_end();
        json_obj_end();
        json_obj("got");
        json_arr("planes");
        for (i = 0; i < 8; i++)
            json_hex(0, c2p_out[i]);
        json_arr_end();
        json_obj_end();
        json_str("why", "Akiko C2P: plane b, bit 31-p must equal bit b of chunky pixel p (pixel 0 = MSB byte of the first longword written)");
        json_obj("in");
        json_int("set", bad);
        json_arr("chunky");
        for (i = 0; i < 8; i++)
            json_hex(0, c2p_in[i]);
        json_arr_end();
        json_obj_end();
        amt_case_close();
    }
    r = amt_hot(c2p_hot, amt_hot_reps(), &first, &badv);
    if (r < 0)
        amt_case("akiko_c2p_hot", AMT_ST_PASS, 0);
    else {
        amt_case_open("akiko_c2p_hot", AMT_ST_FAIL);
        json_obj("expect");
        json_hex("hash", first);
        json_obj_end();
        json_obj("got");
        json_hex("hash", badv);
        json_int("iteration", r);
        json_obj_end();
        json_str("why", "the same C2P conversion gave a different result on a later repetition");
        amt_case_close();
    }
}

/* ------------------------------------------------------------ I2C */
static UBYTE i2c_dir;

static void i2c_delay(void)
{
    int i;
    for (i = 0; i < 6; i++)
        (void)hw_rd8(0xbfe001UL);   /* CIA-A PRA: E-clock synchronised, ~1 us each */
}

/* SCL is driven (push-pull, the 24C08 never stretches the clock); SDA is
 * open drain: a 1 is "released" (direction input), a 0 is driven low.
 * Data, direction, then data again: the latch is set before a pin turns
 * into an output (no glitch), and the second data write is a no-op on the
 * chip (it makes the new direction take effect in emulators that only
 * update the lines on data writes). */
static void i2c_set(int scl, int sda)
{
    UBYTE data = (UBYTE)(scl ? 0x80 : 0x00);
    UBYTE dir = (UBYTE)((i2c_dir & 0x3f) | 0x80 | (sda ? 0 : 0x40));
    hw_wr8(AK_I2C, data);
    hw_wr8(AK_I2CDIR, dir);
    hw_wr8(AK_I2C, data);
    i2c_delay();
}
static int i2c_sda(void) { return (hw_rd8(AK_I2C) >> 6) & 1; }
static int i2c_scl(void) { return (hw_rd8(AK_I2C) >> 7) & 1; }

static void i2c_start(void) { i2c_set(1, 1); i2c_set(1, 0); i2c_set(0, 0); }
static void i2c_rstart(void) { i2c_set(0, 1); i2c_set(1, 1); i2c_set(1, 0); i2c_set(0, 0); }
static void i2c_stop(void) { i2c_set(0, 0); i2c_set(1, 0); i2c_set(1, 1); }

/* returns 1 if the slave acknowledged */
static int i2c_put(UBYTE v)
{
    int i, ack;
    for (i = 7; i >= 0; i--) {
        int b = (v >> i) & 1;
        i2c_set(0, b);
        i2c_set(1, b);
        i2c_set(0, b);
    }
    i2c_set(0, 1);
    i2c_set(1, 1);
    ack = !i2c_sda();
    i2c_set(0, 1);
    return ack;
}

static UBYTE i2c_get(int ack)
{
    int i;
    UBYTE v = 0;
    i2c_set(0, 1);
    for (i = 0; i < 8; i++) {
        i2c_set(1, 1);
        v = (UBYTE)((v << 1) | i2c_sda());
        i2c_set(0, 1);
    }
    i2c_set(0, !ack);
    i2c_set(1, !ack);
    i2c_set(0, !ack);
    i2c_set(0, 1);
    return v;
}

/* random read of n bytes from address 0 of block 0; returns the ack mask */
static int nv_read(UBYTE *buf, int n)
{
    int a = 0, i;
    i2c_start();
    a |= i2c_put(0xa0) ? 1 : 0;
    a |= i2c_put(0x00) ? 2 : 0;
    i2c_rstart();
    a |= i2c_put(0xa1) ? 4 : 0;
    for (i = 0; i < n; i++)
        buf[i] = (a & 4) ? i2c_get(i < n - 1) : 0xff;
    i2c_stop();
    return a;
}

static void t_i2c(void)
{
    UBYTE d0 = hw_rd8(AK_I2C), b1[16], b2[16];
    int idle, a1, a2, nack, i, same = 1;
    i2c_dir = hw_rd8(AK_I2CDIR);
    i2c_set(1, 1);
    idle = i2c_scl() && i2c_sda();
    if (idle)
        amt_case("akiko_i2c_idle", AMT_ST_PASS, 0);
    else {
        amt_case_open("akiko_i2c_idle", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("data", 0xc0, 2);
        json_obj_end();
        json_obj("got");
        json_hexn("data", hw_rd8(AK_I2C) & 0xc0, 2);
        json_obj_end();
        json_str("why", "with SCL and SDA released (direction input) the pulled-up bus must read high");
        amt_case_close();
    }
    if (!idle) {
        amt_case("akiko_nvram_ack", AMT_ST_UNTESTABLE, "I2C bus not idle (see akiko_i2c_idle)");
        amt_case("akiko_nvram_read_stable", AMT_ST_UNTESTABLE, "I2C bus not idle");
        amt_case("akiko_i2c_nack_absent", AMT_ST_UNTESTABLE, "I2C bus not idle");
    } else {
        a1 = nv_read(b1, 16);
        a2 = nv_read(b2, 16);
        if (a1 == 7 && a2 == 7)
            amt_case("akiko_nvram_ack", AMT_ST_PASS, 0);
        else {
            amt_case_open("akiko_nvram_ack", AMT_ST_FAIL);
            json_obj("expect");
            json_str("acks", "A0 word-address A1 all acknowledged");
            json_obj_end();
            json_obj("got");
            json_int("ack_mask_1", a1);
            json_int("ack_mask_2", a2);
            json_obj_end();
            json_str("why", "the 24C08 NVRAM must acknowledge its device address (write and read) and the word address");
            json_str("note", "mask bit0 = A0, bit1 = word address, bit2 = A1");
            amt_case_close();
        }
        for (i = 0; i < 16; i++)
            if (b1[i] != b2[i])
                same = 0;
        if (a1 != 7 || a2 != 7)
            amt_case("akiko_nvram_read_stable", AMT_ST_UNTESTABLE, "no acknowledge (see akiko_nvram_ack)");
        else if (same)
            amt_case("akiko_nvram_read_stable", AMT_ST_PASS, 0);
        else
            amt_case("akiko_nvram_read_stable", AMT_ST_FAIL,
                     "two random reads of NVRAM bytes 0-15 gave different data (EEPROM contents never change on reads)");
        /* no device answers at 1001000x */
        i2c_start();
        nack = !i2c_put(0x91);
        i2c_stop();
        if (nack)
            amt_case("akiko_i2c_nack_absent", AMT_ST_PASS, 0);
        else
        {
            amt_case_open("akiko_i2c_nack_absent", AMT_ST_FAIL);
            json_obj("expect");
            json_str("ack_91h", "nack (SDA high in the 9th clock)");
            json_obj_end();
            json_obj("got");
            json_str("ack_91h", "ack (SDA low)");
            json_obj_end();
            json_obj("in");
            json_hexn("address_byte", 0x91, 2);
            json_str("device", "1001000 read: no device with that address is on the CD32 I2C bus "
                     "(only the 24C08 NVRAM at 1010xxx)");
            json_obj_end();
            json_str("why", "address 91h was acknowledged although no such device is on the CD32 I2C bus "
                     "(I2C spec: an absent slave leaves SDA high in the acknowledge clock)");
            amt_case_close();
        }
    }
    hw_wr8(AK_I2C, (UBYTE)(d0 | 0xc0));
    hw_wr8(AK_I2CDIR, i2c_dir);
    hw_wr8(AK_I2C, (UBYTE)(d0 | 0xc0));
}

void mh_akiko(void)
{
    UWORD id = 0;
    ULONG idl;
    const char *cs = amt_probe_result.chipset;
    if (!cs || cs[0] != 'A') {
        amt_case("akiko_present", AMT_ST_SKIP, "not an AGA machine (Akiko is CD32 only)");
        return;
    }
    if (!mh_try_rd16(AKIKO + 2, &id) || id != 0xcafe) {
        amt_case("akiko_present", AMT_ST_SKIP, "no Akiko: $B80002 does not read $CAFE");
        return;
    }
    amt_case("akiko_present", AMT_ST_PASS, 0);
    idl = hw_rd32(AKIKO);
    amt_check32("akiko_id_long", "id", 0xc0cacafeUL, idl, "Akiko ID register at $B80000 reads $C0CACAFE");
    t_c2p();
    t_i2c();
}

/* ------------------------------------------------------------ CDTV */
void mh_cdtv(void)
{
    LONG i;
    ULONG base = 0, size = 0, k, diff = 0;
    for (i = 0; i < amt_probe_result.n_boards; i++)
        if (amt_probe_result.boards[i].man == 514 && amt_probe_result.boards[i].prod == 3) {
            base = amt_probe_result.boards[i].addr;
            size = amt_probe_result.boards[i].size;
        }
    if (!base) {
        amt_case("cdtv_present", AMT_ST_SKIP, "no CDTV DMAC (autoconfig board 514/3) in the ConfigDev list");
        return;
    }
    amt_case_open("cdtv_present", AMT_ST_PASS);
    json_obj("ctx");
    json_hex("dmac", base);
    json_obj_end();
    amt_case_close();
    amt_check32("cdtv_dmac_size", "size", 0x10000UL, size, "the CDTV DMAC autoconfig board is a 64 KB Zorro II I/O board");
    for (k = 0; k < 0x800; k++) {
        UBYTE a = hw_rd8(0xdc8000UL + k), b = hw_rd8(0xdc8000UL + k);
        if (a != b)
            diff++;
        if ((k & 0xff) == 0)
            amt_watchdog_kick();
    }
    if (!diff)
        amt_case("cdtv_bookmark_stable", AMT_ST_PASS, 0);
    else
        amt_check32("cdtv_bookmark_stable", "unstable_bytes", 0, diff,
                    "battery-backed bookmark RAM at $DC8000: two reads of the same byte differ");
    amt_case("cdtv_front_panel", AMT_ST_SKIP,
             "front panel / IR remote are reached through CD controller commands (cdtv.device), not registers");
}
