/* paula module, disk controller part (bare phase, raw hardware).
 *
 * DF0 (our own boot disk) is only ever READ: the drive is selected, the
 * head is moved to track 0 (counting the steps, so it can be put back),
 * and the tracks holding the bootblock, the payload header and the payload
 * blob are read with DMA (DSKSYNC $4489 + WORDSYNC) and MFM-decoded. The
 * decoded data must match what trackdisk.device read at boot: sector 2 is
 * the header copy the bootblock handed us (amt_hdr), and the blob's CRC-32
 * must equal blob_crc32, which the bootblock checked on the trackdisk data.
 *
 * DF1 is written ONLY when it carries the CONTRACT section 11 signature in
 * block 0 (track 0 sector 0) AND in block 1759 (track 159 sector 10), both
 * raw-read and decoded here, same UUID, total 1760, kind ADF, and is not
 * write-protected. Then track 81 (blocks 891-901, inside the writable
 * region) is written with 11 AmigaDOS sectors holding the section 11 test
 * pattern ("AMTD", LBA, test id 14, seed, xorshift), read back and compared.
 *
 * Restore: disk DMA off, motors off, drives deselected, every head stepped
 * back to the cylinder it was on (trackdisk keeps the head position and
 * the last track in memory), CIA-B PRB as found. DSKSYNC is left at $4489
 * (the OS value). */
#include <exec/devices.h>
#include "paula.h"
#include "hardware_audit.h"

#define PRB_STEP   0x01
#define PRB_DIR    0x02     /* 1 = outward (towards track 0) */
#define PRB_SIDE   0x04     /* 0 = upper head (side 1) */
#define PRB_SEL(d) (0x08 << (d))
#define PRB_SELS   0x78
#define PRB_MTR    0x80     /* 0 = motor on (latched on /SELx falling edge) */
#define PRA_CHNG   0x04     /* all active low */
#define PRA_PROT   0x08
#define PRA_TK0    0x10
#define PRA_RDY    0x20

#define ADK_READ   0x1500   /* MFMPREC | WORDSYNC | FAST */
#define ADK_READNS 0x1100   /* MFMPREC | FAST (no word sync) */
#define ADK_WRITE  0x3100   /* PRECOMP 140 ns | MFMPREC | FAST */
#define MM         0x55555555UL
#define SYNC       0x4489
#define TRACK_WORDS 6250    /* nominal: 300 rpm, 2 us cells */
#define WR_TRACK   81
#define TEST_ID    14

static UBYTE prb, prb0;

static int pa_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static int cur_cyl;
static UWORD *rbuf, *wbuf;

/* Restore state, kept up to date as we go so pa_disk_restore() can put the
 * drives back after an abort too (CONTRACT section 10 rule 4): the drive
 * whose head we move, and the net number of steps from where its head was
 * found (inward positive). rs_known = 0 when that position was lost (no
 * /TRACK0 found): the head is then not moved again. */
static int rs_active;       /* PRB is ours (DDRB checked) */
static int rs_drv = -1, rs_disp, rs_known;

static void rs_begin(int d)
{
    rs_drv = d;
    rs_disp = 0;
    rs_known = 1;
}

/* ------------------------------------------------------------ drive lines */
static void prb_w(UBYTE v)
{
    prb = v;
    pa_cia_wr(CIAB, C_PRB, v);
}

static UBYTE pra(void) { return pa_cia_rd(CIAA, C_PRA); }

static void drv_select(int d, int motor)
{
    prb_w(prb | PRB_SELS);
    prb_w(motor ? (UBYTE)(prb & ~PRB_MTR) : (UBYTE)(prb | PRB_MTR));
    prb_w((UBYTE)(prb & ~PRB_SEL(d)));
}

static void drv_deselect(void) { prb_w(prb | PRB_SELS); }

static void drv_motor_off(int d)
{
    drv_select(d, 0);
    drv_deselect();
}

/* HRM ch.8 drive identification: motor on then off resets the serial ID
 * register; then 32 times select, read /RDY (low = 1), deselect, MSB first */
static ULONG drv_id(int d)
{
    ULONG id = 0;
    int i;
    drv_select(d, 1);
    drv_deselect();
    drv_select(d, 0);
    drv_deselect();
    for (i = 0; i < 32; i++) {
        prb_w((UBYTE)(prb & ~PRB_SEL(d)));
        id <<= 1;
        if (!(pra() & PRA_RDY))
            id |= 1;
        prb_w(prb | PRB_SEL(d));
    }
    return id;
}

static void step1(int out)
{
    UWORD sr;
    UBYTE b = out ? (UBYTE)(prb | PRB_DIR) : (UBYTE)(prb & ~PRB_DIR);
    if (b != prb) {
        /* direction change: let DIR settle a full step period before the
         * pulse (drive data sheets ask for more than the 3 ms step rate
         * after a reversal; trackdisk waits longer around them too) */
        prb_w(b);
        pa_wait_cc(PA_MS(4));
    }
    prb_w((UBYTE)(prb & ~PRB_STEP));
    pa_wait_cc(16);
    sr = amt_get_sr();
    amt_set_sr(0x2700);
    prb_w(prb | PRB_STEP);
    cur_cyl += out ? -1 : 1;
    rs_disp += out ? -1 : 1;
    amt_set_sr(sr);
    pa_wait_cc(PA_MS(4));               /* motion already completed, even on abort */
}

static void settle(void) { pa_wait_cc(PA_MS(20)); }   /* head settle >= 15 ms */

/* step out until /TRACK0; returns the number of steps (the entry cylinder) */
static int seek0(void)
{
    int n = 0;
    while (pra() & PRA_TK0) {
        if (n >= 90)
            return -1;
        step1(1);
        n++;
    }
    cur_cyl = 0;
    settle();
    return n;
}

static void seek(int cyl)
{
    if (cur_cyl == cyl)
        return;
    while (cur_cyl > cyl)
        step1(1);
    while (cur_cyl < cyl)
        step1(0);
    settle();
}

static void set_side(int s)
{
    UBYTE b = s ? (UBYTE)(prb & ~PRB_SIDE) : (UBYTE)(prb | PRB_SIDE);
    if (b != prb) {
        prb_w(b);
        pa_wait_cc(PA_MS(1));
    }
}

static int wait_ready(ULONG *t)
{
    ULONG t0 = pa_now();
    while (pra() & PRA_RDY)
        if (pa_now() - t0 > PA_MS(1000)) {
            *t = pa_now() - t0;
            return 0;
        }
    *t = pa_now() - t0;
    return 1;
}

/* ------------------------------------------------------------ disk DMA */
static volatile UWORD d_blk, d_syn, d_sr1, d_sr5;
static volatile ULONG d_tblk, d_tsyn;

static void hook1(ULONG p)
{
    if (p & INT_DSKBLK) {
        if (!d_blk) {
            d_tblk = pa_now_raw();
            d_sr1 = amt_get_sr();
        }
        d_blk++;
    }
    CREG(R_INTREQ) = (UWORD)p;
    CREG(R_INTREQ) = (UWORD)p;
}

static void hook5(ULONG p)
{
    if (p & INT_DSKSYN) {
        if (!d_syn) {
            d_tsyn = pa_now_raw();
            d_sr5 = amt_get_sr();
        }
        d_syn++;
    }
    CREG(R_INTREQ) = (UWORD)p;
    CREG(R_INTREQ) = (UWORD)p;
}

struct dres {
    UWORD blk, syn, sr1, sr5;
    UWORD bytr_or;           /* OR of all DSKBYTR reads during the transfer */
    ULONG t_syn, t_blk;      /* cck after the (last) DSKLEN write; 0 = none */
};

/* one DMA transfer; dbl = write DSKLEN twice; wait_cc = how long to wait
 * for DSKBLK (the transfer is stopped after that either way) */
static void dsk_dma(UWORD *buf, UWORD words, int write, UWORD adk, UWORD sync, int dbl,
                    ULONG wait_cc, struct dres *r)
{
    ULONG t0;
    UWORD lw = (UWORD)(0x8000 | (write ? 0x4000 : 0) | (words & 0x3fff));
    CREG(R_DSKLEN) = 0x4000;
    CREG(R_ADKCON) = 0x7700;
    CREG(R_ADKCON) = (UWORD)(0x8000 | adk);
    /* Let the data shift register refill with bits sampled at the new
     * ADKCON rate before DSKSYNC is compared and the DMA can start. Between
     * transfers ADKCON FAST is off, so the DD disk under the head is
     * sampled with 4 us cells; MFM gaps and runs of $00/$FF data (a pulse
     * every 4 us) then shift in as all ones, and the register can hold
     * $FFFF, which correctly sampled MFM never contains. 16 cells (32 us)
     * flush it; ~110 us is used. */
    pa_wait_cc(400);
    CREG(R_DSKSYNC) = sync;
    *(volatile ULONG *)(CUSTOM_BASE + R_DSKPTH) = (ULONG)buf;
    CREG(R_INTREQ) = INT_DSKBLK | INT_DSKSYN;
    CREG(R_INTREQ) = INT_DSKBLK | INT_DSKSYN;
    d_blk = d_syn = 0;
    r->bytr_or = 0;
    CREG(R_DMACON) = 0x8210;
    t0 = pa_now();
    CREG(R_DSKLEN) = lw;
    if (dbl)
        CREG(R_DSKLEN) = lw;
    while (!d_blk && pa_now() - t0 < wait_cc)
        r->bytr_or |= CREG(R_DSKBYTR);
    if (write && d_blk)
        pa_wait_cc(PA_MS(1));            /* last word still shifting out */
    CREG(R_DSKLEN) = 0x4000;
    CREG(R_DMACON) = 0x0010;
    r->blk = d_blk;
    r->syn = d_syn;
    r->sr1 = d_sr1;
    r->sr5 = d_sr5;
    r->t_blk = d_blk ? d_tblk - t0 : 0;
    r->t_syn = d_syn ? d_tsyn - t0 : 0;
    CREG(R_ADKCON) = 0x7700;
    CREG(R_DSKSYNC) = SYNC;
}

/* ------------------------------------------------------------ MFM */
struct sinfo {
    UBYTE fmt, trk, sec, gap;
};

/* w: first word after the sync words. 0 ok, 1 header checksum, 2 data */
static int dec_sector(const UWORD *w, struct sinfo *si, ULONG *data)
{
    const ULONG *p = (const ULONG *)w;
    ULONG info = ((p[0] & MM) << 1) | (p[1] & MM), c = 0, st;
    int k;
    si->fmt = (UBYTE)(info >> 24);
    si->trk = (UBYTE)(info >> 16);
    si->sec = (UBYTE)(info >> 8);
    si->gap = (UBYTE)info;
    for (k = 0; k < 10; k++)
        c ^= p[k];
    st = ((p[10] & MM) << 1) | (p[11] & MM);
    if ((c & MM) != st)
        return 1;
    c = 0;
    for (k = 14; k < 270; k++)
        c ^= p[k];
    st = ((p[12] & MM) << 1) | (p[13] & MM);
    if ((c & MM) != st)
        return 2;
    if (data)
        for (k = 0; k < 128; k++)
            data[k] = ((p[14 + k] & MM) << 1) | (p[142 + k] & MM);
    return 0;
}

struct tres {
    UWORD good;               /* sectors decoded with good checksums and fields */
    UWORD hdr_bad, data_bad, dup;   /* vary with the start position: timing only */
    UWORD reads;              /* DMA reads it took (retries: timing only) */
    UWORD bad_fields;         /* good checksums but wrong fmt/track/sector/gap */
    struct sinfo first_bad;
};

static ULONG secdata[11][128];
static ULONG tmpsec[128];

/* returns 1 if the checksums were good (a whole sector is there) */
static int dec_one(const UWORD *w, int track, struct tres *r)
{
    struct sinfo si;
    int rc = dec_sector(w, &si, tmpsec), k;
    if (rc == 1) {
        r->hdr_bad++;
        return 0;
    }
    if (rc == 2) {
        r->data_bad++;
        return 0;
    }
    if (si.fmt != 0xff || si.trk != track || si.sec > 10 || si.gap < 1 || si.gap > 11) {
        if (!r->bad_fields)
            r->first_bad = si;
        r->bad_fields++;
        return 1;
    }
    if (r->good & (1u << si.sec)) {
        r->dup++;
        return 1;
    }
    r->good |= (UWORD)(1u << si.sec);
    for (k = 0; k < 128; k++)
        secdata[si.sec][k] = tmpsec[k];
    return 1;
}

/* every sector start in buf: after a run of sync words, and (word-synced
 * read) possibly at buf[0] when DMA started on the second sync word */
static void tres_clear(struct tres *r)
{
    r->good = r->hdr_bad = r->data_bad = r->dup = r->bad_fields = r->reads = 0;
}

/* adds to r (good sectors of several reads are merged) */
static void dec_track(const UWORD *buf, int n, int track, struct tres *r)
{
    int i = 0;
    if (n >= 540 && buf[0] != SYNC)
        dec_one(buf, track, r);
    while (i < n) {
        if (buf[i] != SYNC) {
            i++;
            continue;
        }
        while (i < n && buf[i] == SYNC)
            i++;
        if (i + 540 > n)
            break;
        if (dec_one(buf + i, track, r))
            i += 540;
    }
}

/* ---- MFM encoder (write buffer) */
static UWORD *wp;
static ULONG wprev;           /* last cell written (data bit 0 of the last long) */

static void put_raw(UWORD w)
{
    *wp++ = w;
    wprev = w & 1;
}

static void put_data(ULONG x)  /* x: data bits in the $55555555 positions */
{
    ULONG clk = ~((x << 1) | (x >> 1) | (wprev << 31)) & 0xaaaaaaaaUL, m = x | clk;
    *wp++ = (UWORD)(m >> 16);
    *wp++ = (UWORD)m;
    wprev = m & 1;
}

static ULONG pat[128];
static void pat_block(ULONG *dst, ULONG lba)
{
    UWORD *w = (UWORD *)dst;
    ULONG seed = amt_par.seed, x = (lba * 0x9e3779b1UL) ^ ((ULONG)TEST_ID * 0x85ebca6bUL) ^ seed;
    int i;
    if (!x)
        x = 1;
    w[0] = 0x414d;
    w[1] = 0x5444;
    w[2] = (UWORD)(lba >> 16);
    w[3] = (UWORD)lba;
    w[4] = 0;
    w[5] = TEST_ID;
    w[6] = (UWORD)(seed >> 16);
    w[7] = (UWORD)seed;
    for (i = 8; i < 256; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w[i] = (UWORD)(x >> 7);
    }
}

/* lead gap, 11 sectors, short tail; returns the word count */
static UWORD build_track(int track)
{
    int s, k;
    wp = wbuf;
    wprev = 0;
    for (k = 0; k < 20; k++)
        put_data(0);
    for (s = 0; s < 11; s++) {
        ULONG info = 0xff000000UL | ((ULONG)track << 16) | ((ULONG)s << 8) | (ULONG)(11 - s);
        ULONG hc, dc = 0;
        pat_block(pat, (ULONG)track * 11 + s);
        put_data(0);
        put_raw(SYNC);
        put_raw(SYNC);
        put_data((info >> 1) & MM);
        put_data(info & MM);
        for (k = 0; k < 8; k++)
            put_data(0);
        hc = (((info >> 1) & MM) ^ (info & MM)) & MM;
        put_data((hc >> 1) & MM);
        put_data(hc & MM);
        for (k = 0; k < 128; k++)
            dc ^= ((pat[k] >> 1) & MM) ^ (pat[k] & MM);
        dc &= MM;
        put_data((dc >> 1) & MM);
        put_data(dc & MM);
        for (k = 0; k < 128; k++)
            put_data((pat[k] >> 1) & MM);
        for (k = 0; k < 128; k++)
            put_data(pat[k] & MM);
    }
    for (k = 0; k < 4; k++)
        put_data(0);
    return (UWORD)(wp - wbuf);
}

/* ------------------------------------------------------------ reporting */
static void j_tres(const char *key, const struct tres *t)
{
    json_obj(key);
    json_hexn("good", t->good, 4);
    json_int("bad_fields", t->bad_fields);
    if (t->bad_fields) {
        json_hexn("bad_fmt", t->first_bad.fmt, 2);
        json_int("bad_trk", t->first_bad.trk);
        json_int("bad_sec", t->first_bad.sec);
        json_int("bad_gap", t->first_bad.gap);
    }
    json_obj_end();
}

static void j_tres_timing(const struct tres *t)
{
    json_int("reads", t->reads);
    json_int("hdr_csum_bad", t->hdr_bad);
    json_int("data_csum_bad", t->data_bad);
    json_int("dups", t->dup);
}

/* the DSKBLK count is a result; the DSKSYN count (depends on where in the
 * revolution the read started), the times and the DSKBYTR samples vary
 * run to run, so they go under "timing" (not fingerprinted) */
static void j_dres(const struct dres *r)
{
    json_int("dskblk", r->blk);
    json_obj("timing");
    json_int("dsksyn", r->syn);
    json_int("t_dsksyn_cck", (long)r->t_syn);
    json_int("t_dskblk_cck", (long)r->t_blk);
    json_hexn("dskbytr_or", r->bytr_or, 4);
    json_obj_end();
}

static void case_skip_list(const char *const *ids, int n, int st, const char *why)
{
    int i;
    for (i = 0; i < n; i++)
        amt_case(ids[i], st, why);
}

static int canary_intact(const UWORD *b, int n, UWORD v)
{
    int i;
    for (i = 0; i < n; i++)
        if (b[i] != (UWORD)(v + i))
            return 0;
    return 1;
}

static void canary_fill(UWORD *b, int n, UWORD v)
{
    int i;
    for (i = 0; i < n; i++)
        b[i] = (UWORD)(v + i);
}

static ULONG us_to_cc(ULONG us) { return (ULONG)(((unsigned long long)us * amt_cck_hz()) / 1000000ULL); }

/* ------------------------------------------------------------ DF0 */
static int df0_entry = -1;
static struct dres first_read;
static int first_read_done;

static void dsklen_tests(void)
{
    struct dres r;
    int intact1, changed2;
    UWORD blk1;

    /* single DSKLEN write: no transfer; then the second write starts it */
    canary_fill(rbuf, 256, 0x1234);
    CREG(R_DSKLEN) = 0x4000;
    CREG(R_ADKCON) = 0x7700;
    CREG(R_ADKCON) = 0x8000 | ADK_READNS;
    CREG(R_DSKSYNC) = SYNC;
    *(volatile ULONG *)(CUSTOM_BASE + R_DSKPTH) = (ULONG)rbuf;
    CREG(R_INTREQ) = INT_DSKBLK | INT_DSKSYN;
    CREG(R_INTREQ) = INT_DSKBLK | INT_DSKSYN;
    d_blk = d_syn = 0;
    CREG(R_DMACON) = 0x8210;
    CREG(R_DSKLEN) = 0x8000 | 256;
    pa_wait_cc(PA_MS(40));
    blk1 = d_blk;
    intact1 = canary_intact(rbuf, 256, 0x1234);
    {
        ULONG t0;
        CREG(R_DSKLEN) = 0x8000 | 256;
        t0 = pa_now();
        while (!d_blk && pa_now() - t0 < PA_MS(100))
            ;
        r.t_blk = d_blk ? d_tblk - t0 : 0;
    }
    r.blk = d_blk;
    CREG(R_DSKLEN) = 0x4000;
    CREG(R_DMACON) = 0x0010;
    CREG(R_ADKCON) = 0x7700;
    changed2 = !canary_intact(rbuf, 256, 0x1234);
    if (blk1 == 0 && intact1)
        amt_case("dsklen_single_write", AMT_ST_PASS, 0);
    else {
        amt_case_open("dsklen_single_write", AMT_ST_FAIL);
        json_obj("in");
        json_hexn("dsklen", 0x8000 | 256, 4);
        json_hexn("adkcon", ADK_READNS, 4);
        json_int("wait_ms", 40);
        json_obj_end();
        json_obj("expect");
        json_int("dskblk", 0);
        json_int("buffer_untouched", 1);
        json_obj_end();
        json_obj("got");
        json_int("dskblk", blk1);
        json_int("buffer_untouched", intact1);
        json_obj_end();
        json_str("why", "HRM ch.8: disk DMA starts only when DSKLEN is written twice in a row "
                 "with DMAEN set; a single write must not transfer anything (256 words without "
                 "word sync would take ~8 ms)");
        amt_case_close();
    }
    if (r.blk == 1 && changed2) {
        amt_case_open("dsklen_double_write", AMT_ST_PASS);
        json_obj("timing");
        json_int("t_dskblk_cck", (long)r.t_blk);
        json_int("nominal_cck", (long)us_to_cc(256UL * 32));
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open("dsklen_double_write", AMT_ST_FAIL);
        json_obj("expect");
        json_int("dskblk", 1);
        json_int("buffer_written", 1);
        json_obj_end();
        json_obj("got");
        json_int("dskblk", r.blk);
        json_int("buffer_written", changed2);
        json_obj_end();
        json_str("why", "HRM ch.8: the second DSKLEN write with DMAEN starts the read DMA (no word "
                 "sync: at once); 256 words then arrive in ~8 ms and DSKBLK (INTREQ bit 1, "
                 "level 1) follows");
        amt_case_close();
    }

    /* the transfer is exactly DSKLEN words: nothing written after them */
    {
        int k, last = -1, over = -1;
        canary_fill(rbuf, 1024 + 64, 0x2468);
        dsk_dma(rbuf, 1024, 0, ADK_READ, SYNC, 1, PA_MS(400), &r);
        for (k = 0; k < 1024; k++)
            if (rbuf[k] != (UWORD)(0x2468 + k))
                last = k;
        for (k = 0; k < 64; k++)
            if (rbuf[1024 + k] != (UWORD)(0x2468 + 1024 + k)) {
                over = k;
                break;
            }
        if (r.blk == 1 && over < 0 && last >= 1000) {
            amt_case_open("dsklen_exact_count", AMT_ST_PASS);
            json_obj("timing");
            j_dres(&r);
            json_obj_end();
            amt_case_close();
        } else {
            amt_case_open("dsklen_exact_count", AMT_ST_FAIL);
            json_obj("in");
            json_int("words", 1024);
            json_hexn("adkcon", ADK_READ, 4);
            json_int("guard_words", 64);
            json_obj_end();
            json_obj("expect");
            json_int("dskblk", 1);
            json_str("written", "words 0..1023 (last changed word >= 1000)");
            json_int("first_guard_hit", -1);
            json_obj_end();
            json_obj("got");
            json_int("last_changed_word", last);
            json_int("first_guard_hit", over);
            if (over >= 0)
                json_hexn("guard_word", rbuf[1024 + over], 4);
            j_dres(&r);
            json_obj_end();
            json_str("why", "HRM ch.8: a disk DMA transfers exactly DSKLEN (bits 13-0) words to DSKPT "
                     "and then raises DSKBLK; the 64 words after the buffer must stay untouched");
            amt_case_close();
        }
    }

    /* WORDSYNC with a sync word that cannot occur in MFM ($FFFF has adjacent
     * 1 cells): the transfer never starts */
    canary_fill(rbuf, 256, 0x5678);
    dsk_dma(rbuf, 256, 0, ADK_READ, 0xffff, 1, PA_MS(450), &r);
    intact1 = canary_intact(rbuf, 256, 0x5678);
    if (r.blk == 0 && r.syn == 0 && intact1)
        amt_case("wordsync_waits", AMT_ST_PASS, 0);
    else {
        amt_case_open("wordsync_waits", AMT_ST_FAIL);
        json_obj("in");
        json_hexn("dsksync", 0xffff, 4);
        json_hexn("adkcon", ADK_READ, 4);
        json_int("wait_ms", 450);
        json_obj_end();
        json_obj("expect");
        json_int("dskblk", 0);
        json_int("dsksyn", 0);
        json_int("buffer_untouched", 1);
        json_obj_end();
        json_obj("got");
        json_int("dskblk", r.blk);
        json_int("dsksyn", r.syn);
        json_int("buffer_untouched", intact1);
        json_obj("timing");
        json_int("t_dsksyn_cck", (long)r.t_syn);
        json_int("t_dskblk_cck", (long)r.t_blk);
        json_hexn("dskbytr_or", r.bytr_or, 4);
        json_obj_end();
        json_arr("buf");
        {
            int k;
            for (k = 0; k < 8; k++)
                json_hexn(NULL, rbuf[k], 4);
        }
        json_arr_end();
        json_obj_end();
        json_str("why", "HRM ch.8: with ADKCON WORDSYNC set, read DMA waits until the data "
                 "matches DSKSYNC; $FFFF never occurs in MFM (no two adjacent 1 cells), so in "
                 "2+ revolutions nothing may be transferred and DSKSYN must not fire");
        amt_case_close();
    }

    /* WORDSYNC off: the transfer starts at once */
    canary_fill(rbuf, 256, 0x9abc);
    dsk_dma(rbuf, 256, 0, ADK_READNS, SYNC, 1, PA_MS(100), &r);
    changed2 = !canary_intact(rbuf, 256, 0x9abc);
    if (r.blk == 1 && changed2) {
        amt_case_open("wordsync_off", AMT_ST_PASS);
        json_obj("timing");
        j_dres(&r);
        json_int("nominal_cck", (long)us_to_cc(256UL * 32));
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open("wordsync_off", AMT_ST_FAIL);
        json_obj("expect");
        json_int("dskblk", 1);
        json_int("buffer_written", 1);
        json_obj_end();
        json_obj("got");
        json_int("buffer_written", changed2);
        j_dres(&r);
        json_obj_end();
        json_str("why", "HRM ch.8: without WORDSYNC the read DMA starts at once; 256 words (~8 ms "
                 "at 2 us/cell) must arrive and DSKBLK fire within 100 ms");
        amt_case_close();
    }
}

static void first_read_cases(const struct tres *t0)
{
    const struct dres *r = &first_read;
    int ok;
    /* DSKBLK: level 1, exactly once */
    ok = r->blk == 1 && (r->sr1 & 0x0700) == 0x0100;
    amt_case_open("dskblk_irq", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    json_obj("timing");
    j_dres(r);
    json_obj_end();
    if (!ok) {
        json_obj("expect");
        json_int("dskblk", 1);
        json_hexn("sr_mask", 0x0100, 4);
        json_obj_end();
        json_obj("got");
        json_int("dskblk", r->blk);
        json_hexn("sr_mask", r->sr1 & 0x0700, 4);
        json_obj_end();
        json_str("why", "HRM ch.7/8: one DSKBLK request (INTREQ bit 1) at the end of a disk DMA "
                 "block, taken as a level-1 interrupt (SR mask 1 in the handler)");
    }
    amt_case_close();

    /* DSKSYN: level 5, at least one per sector; the first before DSKBLK */
    ok = r->syn >= 11 && (r->sr5 & 0x0700) == 0x0500 && r->t_syn && r->t_syn < r->t_blk;
    amt_case_open("dsksyn_irq", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    json_obj("in");
    json_int("words", PA_RDWORDS);
    json_obj_end();
    json_obj("timing");
    j_dres(r);
    json_obj_end();
    if (!ok) {
        json_obj("expect");
        json_str("dsksyn", ">=11");
        json_hexn("sr_mask", 0x0500, 4);
        json_str("order", "first DSKSYN before DSKBLK");
        json_obj_end();
        json_obj("got");
        json_int("dsksyn", r->syn);
        json_hexn("sr_mask", r->sr5 & 0x0700, 4);
        json_int("t_dsksyn", (long)r->t_syn);
        json_int("t_dskblk", (long)r->t_blk);
        json_obj_end();
        json_str("why", "HRM ch.7/8: DSKSYN (INTREQ bit 12, level 5) is requested whenever the "
                 "input matches DSKSYNC; a word-synced read of 7000 words (>1 revolution) sees "
                 "every sector's $4489 sync, and the first match starts the DMA, so it comes "
                 "before DSKBLK");
    }
    amt_case_close();

    /* DSKBYTR during a read DMA */
    ok = (r->bytr_or & 0x8000) && (r->bytr_or & 0x4000) && !(r->bytr_or & 0x2000);
    amt_case_open("dskbytr_read", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_int("dskbyt_seen", 1);
        json_int("dmaon_seen", 1);
        json_int("diskwrite_seen", 0);
        json_obj_end();
        json_obj("got");
        json_hexn("dskbytr_or", r->bytr_or, 4);
        json_obj_end();
        json_str("why", "HRM ch.8 DSKBYTR: while a read DMA runs, DSKBYT (bit 15) is set for "
                 "every byte assembled, DMAON (bit 14) is 1, DISKWRITE (bit 13) is 0");
    }
    amt_case_close();
    (void)t0;
}

/* read track t of the selected drive into rbuf, decoded into secdata.
 * Like trackdisk.device, a read that misses sectors (checksum errors: a
 * real medium can have soft errors) is retried, up to 4 reads; the good
 * sectors of all reads are merged. *r is the first read's result. */
#define READ_TRIES 4
static void read_track(int t, struct tres *tr, struct dres *r)
{
    struct dres rr;
    int k;
    seek(t >> 1);
    set_side(t & 1);
    tres_clear(tr);
    for (k = 0; k < READ_TRIES; k++) {
        amt_watchdog_kick();            /* each try is bounded (1.2 s) */
        dsk_dma(rbuf, PA_RDWORDS, 0, ADK_READ, SYNC, 1, PA_MS(1200), k ? &rr : r);
        tr->reads++;
        if (!(k ? rr.blk : r->blk))
            break;                      /* no DSKBLK: retrying will not help */
        dec_track(rbuf, PA_RDWORDS, t, tr);
        if (tr->good == 0x7ff || tr->bad_fields)
            break;
    }
}

static ULONG bb_sum(const ULONG *b)
{
    ULONG s = 0, x;
    int i;
    for (i = 0; i < 256; i++) {
        x = s + b[i];
        if (x < s)
            x++;
        s = x;
    }
    return s;
}

static ULONG secdata2[128];         /* decoded sector 2 (LBA 2) */

static void df0_read_tests(void)
{
    static ULONG bb[256];
    const UBYTE *hdr = (const UBYTE *)amt_hdr;
    ULONG crc_len = amt_hdr->alloc_size + amt_hdr->data_size + amt_hdr->reloc_count * 4, done = 0, crc = 0;
    ULONG last_lba = 2 + (crc_len + 511) / 512, t_total = 0, retries = 0;
    int ntracks = (int)(last_lba / 11) + 1, t, s, k, bad_t = -1, hdr_diff = -1, have = 0;
    struct tres tr, bad_tr;
    struct dres r, bad_r;
    ULONG t0;

    t0 = pa_now();
    for (t = 0; t < ntracks; t++) {
        read_track(t, &tr, &r);
        retries += tr.reads - 1;
        amt_watchdog_kick();
        if (t == 0) {
            first_read = r;
            first_read_done = 1;
            first_read_cases(&tr);
        }
        if (tr.good != 0x7ff || tr.bad_fields) {
            if (bad_t < 0) {
                bad_t = t;
                bad_tr = tr;
                bad_r = r;
            }
        }
        for (s = 0; s < 11; s++) {
            ULONG lba = (ULONG)t * 11 + s;
            const UBYTE *d = (const UBYTE *)secdata[s];
            if (!(tr.good & (1u << s)))
                continue;
            if (lba < 3)
                have |= 1 << lba;
            if (lba < 2)
                for (k = 0; k < 128; k++)
                    bb[lba * 128 + k] = secdata[s][k];
            else if (lba == 2) {
                for (k = 0; k < 128; k++)
                    secdata2[k] = secdata[s][k];
                for (k = 0; k < 512; k++)
                    if (d[k] != hdr[k]) {
                        hdr_diff = k;
                        break;
                    }
            } else if (done < crc_len) {
                ULONG n = crc_len - done;
                if (n > 512)
                    n = 512;
                crc = crc32_update(crc, d, n);
                done += n;
            }
        }
    }
    t_total = pa_now() - t0;
    amt_perf("df0_raw_track_read", (ULONG)ntracks, t_total);

    if (bad_t < 0) {
        amt_case_open("df0_raw_sectors", AMT_ST_PASS);
        json_obj("in");
        json_int("tracks", ntracks);
        json_obj_end();
        json_obj("timing");
        json_int("retries", (long)retries);
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open("df0_raw_sectors", AMT_ST_FAIL);
        json_obj("in");
        json_int("tracks", ntracks);
        json_int("track", bad_t);
        json_int("cyl", bad_t >> 1);
        json_int("side", bad_t & 1);
        json_obj_end();
        json_obj("expect");
        json_hexn("good", 0x7ff, 4);
        json_int("bad_fields", 0);
        json_obj_end();
        json_obj("got");
        j_tres("track", &bad_tr);
        j_dres(&bad_r);
        json_obj("timing");
        j_tres_timing(&bad_tr);
        json_obj_end();
        json_obj_end();
        json_str("why", "raw word-synced read of a track of our AmigaDOS boot disk must give all "
                 "11 sectors (mask $7FF) with good header and data checksums (XOR of the MFM "
                 "longs AND $55555555), format byte $FF, the track number we stepped/side-"
                 "selected to (cyl*2+side, side = /DSKSIDE low), sectors-to-gap 1..11");
        amt_case_close();
    }

    /* Kickstart 2+ can boot from DF1-DF3 through trackdisk.device too, and
     * DF0 then holds some other disk. Sector 2 read cleanly but without our
     * payload header ("AMTP" magic, or another build: build id at offset
     * 128-191 differs) means DF0 is not the disk we booted from: the
     * content cases are untestable, not failed. */
    if ((have & 4) && hdr_diff >= 0) {
        const UBYTE *d2 = (const UBYTE *)secdata2;
        int other = d2[0] != 'A' || d2[1] != 'M' || d2[2] != 'T' || d2[3] != 'P';
        for (k = 128; !other && k < 192; k++)
            if (d2[k] != hdr[k])
                other = 1;
        if (other) {
            static const char *const cids[] = { "df0_bootblock", "df0_payload_header", "df0_blob_crc" };
            case_skip_list(cids, 3, AMT_ST_UNTESTABLE, "DF0 holds another disk: sector 2 decodes "
                           "cleanly but is not our payload header (no AMTP magic, or another build "
                           "id): the machine booted from another drive (Kickstart 2+ boots from "
                           "DF1-DF3 through trackdisk.device as well)");
            return;
        }
    }

    /* bootblock: DOS\0 and the checksum */
    if ((have & 3) != 3)
        amt_case("df0_bootblock", AMT_ST_FAIL, "sectors 0-1 not decoded (see df0_raw_sectors)");
    else {
        ULONG sum = bb_sum(bb);
        int ok = (bb[0] >> 8) == 0x444f53UL && sum == 0xffffffffUL;
        amt_case_open("df0_bootblock", ok ? AMT_ST_PASS : AMT_ST_FAIL);
        if (!ok) {
            json_obj("expect");
            json_hexn("dostype_hi", 0x444f53, 6);
            json_hex("sum", 0xffffffffUL);
            json_obj_end();
            json_obj("got");
            json_hex("long0", bb[0]);
            json_hex("sum", sum);
            json_obj_end();
            json_str("why", "sectors 0-1 decoded from raw MFM must be the bootblock: 'DOS', and "
                     "the sum with end-around carry of its 256 longs = $FFFFFFFF");
        }
        amt_case_close();
    }

    if (hdr_diff < 0 && (have & 4))
        amt_case("df0_payload_header", AMT_ST_PASS, 0);
    else {
        amt_case_open("df0_payload_header", AMT_ST_FAIL);
        json_obj("got");
        json_int("first_diff_offset", hdr_diff);
        if (hdr_diff >= 0) {
            json_hexn("raw", ((const UBYTE *)secdata[2])[hdr_diff], 2);
            json_hexn("trackdisk", hdr[hdr_diff], 2);
        }
        json_obj_end();
        json_str("why", "sector 2 decoded from raw MFM must equal, byte for byte, the payload "
                 "header the bootblock read with trackdisk.device (amt_hdr)");
        amt_case_close();
    }

    if (done == crc_len && crc == amt_hdr->blob_crc32) {
        amt_case_open("df0_blob_crc", AMT_ST_PASS);
        json_obj("in");
        json_int("bytes", (long)crc_len);
        json_int("tracks", ntracks);
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open("df0_blob_crc", AMT_ST_FAIL);
        json_obj("in");
        json_int("bytes", (long)crc_len);
        json_int("tracks", ntracks);
        json_obj_end();
        json_obj("expect");
        json_hex("crc32", amt_hdr->blob_crc32);
        json_int("bytes", (long)crc_len);
        json_obj_end();
        json_obj("got");
        json_hex("crc32", crc);
        json_int("bytes", (long)done);
        json_obj_end();
        json_str("why", "the payload blob (sector 3 on) decoded from raw MFM must have the CRC-32 "
                 "the bootblock verified on the same bytes read through trackdisk.device");
        amt_case_close();
    }
}

/* back on the entry cylinder? read and check the track number */
static void head_check(const char *id, int entry)
{
    struct tres tr;
    struct dres r;
    int side = (prb0 & PRB_SIDE) ? 0 : 1, t = entry * 2 + side, ok;
    seek(entry);
    set_side(side);
    read_track(t, &tr, &r);
    ok = tr.good != 0 && !tr.bad_fields;
    if (ok) {
        amt_case_open(id, AMT_ST_PASS);
        json_obj("timing");
        json_int("entry_cyl", entry);
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open(id, AMT_ST_FAIL);
        json_obj("expect");
        json_str("headers", "track field = entry cylinder * 2 + side");
        json_obj_end();
        json_obj("got");
        j_tres("track", &tr);
        json_int("expected_track", t);
        json_obj_end();
        json_str("why", "after stepping back by the counted number of steps the head must be on "
                 "the cylinder it was found on (trackdisk.device keeps its own head position)");
        amt_case_close();
    }
}

static void df0_tests(void)
{
    static const char *const ids[] = {
        "df0_motor_ready", "df0_seek_track0", "df0_track0_sensor", "df0_disk_change",
        "dsklen_single_write", "dsklen_double_write", "dsklen_exact_count", "wordsync_waits",
        "wordsync_off", "dskblk_irq", "dsksyn_irq", "dskbytr_read", "df0_raw_sectors", "df0_bootblock",
        "df0_payload_header", "df0_blob_crc", "df0_head_restored"
    };
    ULONG trdy;
    int ok, a, b, c;
    const char *bn = 0;

    if (amt_bootio && amt_bootio->io_Device)
        bn = amt_bootio->io_Device->dd_Library.lib_Node.ln_Name;
    if (!bn || !pa_streq(bn, "trackdisk.device")) {
        case_skip_list(ids, 17, AMT_ST_SKIP, "not booted from a floppy through trackdisk.device "
                       "(CD/CLI start: bootio NULL, or another boot device): DF0 is not known to "
                       "hold our disk, so it is not read");
        return;
    }
    drv_select(0, 1);
    ok = wait_ready(&trdy);
    amt_case_open("df0_motor_ready", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    json_obj("timing");
    json_int("t_ready_cck", (long)trdy);
    json_obj_end();
    if (!ok) {
        json_obj("expect");
        json_int("dskrdy_low", 1);
        json_obj_end();
        json_obj("got");
        json_hexn("ciaa_pra", pra(), 2);
        json_obj_end();
        json_str("why", "HRM ch.8: with DF0 selected and its motor on, /DSKRDY (CIA-A PRA bit 5) "
                 "goes low when the motor is up to speed (well within 1 s)");
    }
    amt_case_close();

    rs_begin(0);
    df0_entry = seek0();
    if (df0_entry < 0) {
        rs_known = 0;
        amt_case("df0_seek_track0", AMT_ST_FAIL, "no /TRACK0 (CIA-A PRA bit 4) after 90 outward "
                 "steps (DIR=1, STEP pulses 4 ms apart)");
        case_skip_list(ids + 2, 15, AMT_ST_UNTESTABLE, "DF0 head position unknown (no /TRACK0)");
        drv_motor_off(0);
        return;
    }
    amt_case_open("df0_seek_track0", AMT_ST_PASS);
    json_obj("timing");
    json_int("entry_cyl", df0_entry);
    json_obj_end();
    amt_case_close();

    /* /TRACK0 follows the head */
    a = pra() & PRA_TK0;
    step1(0);
    settle();                           /* head settled before the sensor read */
    b = pra() & PRA_TK0;
    step1(1);
    settle();
    c = pra() & PRA_TK0;
    ok = a == 0 && b != 0 && c == 0;
    if (ok)
        amt_case("df0_track0_sensor", AMT_ST_PASS, 0);
    else {
        amt_case_open("df0_track0_sensor", AMT_ST_FAIL);
        json_obj("expect");
        json_str("tk0", "low at cyl 0, high at cyl 1, low again at cyl 0");
        json_obj_end();
        json_obj("got");
        json_int("cyl0", a ? 1 : 0);
        json_int("cyl1", b ? 1 : 0);
        json_int("cyl0_again", c ? 1 : 0);
        json_obj_end();
        json_str("why", "HRM ch.8: /DSKTRACK0 (CIA-A PRA bit 4) is low only while the selected "
                 "drive's head is on cylinder 0; one inward step (DIR=0) leaves it");
        amt_case_close();
    }

    /* /CHNG: a disk is in and we have stepped, so no change is latched */
    if (pra() & PRA_CHNG)
        amt_case("df0_disk_change", AMT_ST_PASS, 0);
    else {
        amt_case_open("df0_disk_change", AMT_ST_FAIL);
        json_obj("got");
        json_hexn("ciaa_pra", pra(), 2);
        json_obj_end();
        json_str("why", "HRM ch.8: /DSKCHANGE (CIA-A PRA bit 2) is reset by a step pulse while a "
                 "disk is inserted; DF0 holds our boot disk and was just stepped, so it must "
                 "read high");
        amt_case_close();
    }

    set_side(0);
    dsklen_tests();
    df0_read_tests();
    head_check("df0_head_restored", df0_entry);
    drv_motor_off(0);
    if (rs_disp == 0)
        rs_drv = -1;                    /* back where it was found */
}

/* ------------------------------------------------------------ DF1 */
struct sig {
    UBYTE uuid[16];
    ULONG lba, total, bsize, first, kind;
};

static int sig_parse(const UBYTE *b, struct sig *s)
{
    static const char magic[8] = { 'A', 'M', 'T', 'O', 'W', 'N', '0', '1' };
    int i;
    ULONG crc;
    for (i = 0; i < 8; i++)
        if (b[i] != (UBYTE)magic[i])
            return 0;
    crc = ((ULONG)b[508] << 24) | ((ULONG)b[509] << 16) | ((ULONG)b[510] << 8) | b[511];
    if (crc32_update(0, b, 508) != crc)
        return 0;
    for (i = 0; i < 16; i++)
        s->uuid[i] = b[8 + i];
#define BE32(o) (((ULONG)b[o] << 24) | ((ULONG)b[o + 1] << 16) | ((ULONG)b[o + 2] << 8) | b[o + 3])
    s->lba = BE32(24);
    s->total = BE32(28);
    s->bsize = BE32(32);
    s->first = BE32(36);
    s->kind = BE32(40);
#undef BE32
    return 1;
}

static const char *const df1_ids[] = {
    "df1_signature", "df1_write_dskblk", "dskbytr_write", "df1_write_readback", "df1_head_restored"
};

static void df1_tests(ULONG id1)
{
    struct sig s0, s1;
    struct tres tr;
    struct dres r;
    ULONG trdy, first_lba = (ULONG)WR_TRACK * 11, words, nominal, speed;
    int entry, i, ok, bad_sec = -1, bad_off = -1;

    if (id1 == 0) {
        case_skip_list(df1_ids, 5, AMT_ST_SKIP, "no DF1 drive (drive ID $00000000)");
        return;
    }
    if (id1 != 0xffffffffUL && id1 != 0xaaaaaaaaUL) {
        /* $55555555 = 5.25" 40-track drive: track 159 does not exist (the
         * head would hit its stop); anything else is unknown */
        case_skip_list(df1_ids, 5, AMT_ST_SKIP, "DF1 is not a 3.5\" drive (ID not $FFFFFFFF/"
                       "$AAAAAAAA, HRM ch.8; $55555555 = 5.25\" 40 tracks): not read or written");
        return;
    }
    drv_select(1, 1);
    if (!wait_ready(&trdy)) {
        drv_motor_off(1);
        case_skip_list(df1_ids, 5, AMT_ST_SKIP, "DF1 not ready (/DSKRDY high 1 s after motor on): no disk");
        return;
    }
    rs_begin(1);
    entry = seek0();
    if (entry < 0) {
        rs_known = 0;
        drv_motor_off(1);
        case_skip_list(df1_ids, 5, AMT_ST_UNTESTABLE, "DF1: no /TRACK0 after 90 outward steps");
        return;
    }
    if (entry == 0) {               /* make sure a step pulse has cleared /CHNG */
        step1(0);
        step1(1);
        settle();
    }
    if (!(pra() & PRA_CHNG)) {
        seek(entry);
        drv_motor_off(1);
        case_skip_list(df1_ids, 5, AMT_ST_SKIP, "DF1: no disk inserted (/DSKCHANGE stays low after a step)");
        return;
    }
    /* ownership: block 0 = track 0 sector 0, block 1759 = track 159 sector 10 */
    read_track(0, &tr, &r);
    ok = (tr.good & 1) && sig_parse((const UBYTE *)secdata[0], &s0) && s0.lba == 0 &&
         s0.total == 1760 && s0.bsize == 512 && s0.kind == 1;
    if (ok) {
        read_track(159, &tr, &r);
        ok = (tr.good & 0x400) && sig_parse((const UBYTE *)secdata[10], &s1) && s1.lba == 1759 &&
             hw_owner_match(s0.total, s0.first, s0.bsize, s0.kind,
                            s1.total, s1.first, s1.bsize, s1.kind) &&
             s0.first < s0.total - 1;
        for (i = 0; ok && i < 16; i++)
            if (s0.uuid[i] != s1.uuid[i])
                ok = 0;
    }
    if (!ok) {
        seek(entry);
        drv_motor_off(1);
        case_skip_list(df1_ids, 5, AMT_ST_SKIP, "DF1 is not a test-owned medium (no matching "
                       "AMTOWN01 signature in blocks 0 and 1759, CONTRACT section 11): not written");
        return;
    }
    if (first_lba < (s0.first < 2 ? 2 : s0.first) || first_lba + 11 > s0.total - 1) {
        seek(entry);
        drv_motor_off(1);
        case_skip_list(df1_ids, 5, AMT_ST_SKIP, "DF1 test track 81 is outside the signed writable region");
        return;
    }
    amt_case("df1_signature", AMT_ST_PASS, 0);
    if (!(pra() & PRA_PROT)) {
        seek(entry);
        drv_motor_off(1);
        case_skip_list(df1_ids + 1, 4, AMT_ST_UNTESTABLE, "DF1 is write-protected (/DSKPROT low)");
        return;
    }
    /* safety: the head must really be on track 81 before writing */
    read_track(WR_TRACK, &tr, &r);
    if (tr.good == 0 || tr.bad_fields) {
        amt_case_open("df1_write_dskblk", AMT_ST_FAIL);
        json_obj("got");
        j_tres("track", &tr);
        j_dres(&r);
        json_obj_end();
        json_str("why", "before writing, track 81 of DF1 (cyl 40, side 1) did not read back with "
                 "track number 81 in its headers: positioning (STEP/DIR/SIDE) wrong; not written");
        amt_case_close();
        case_skip_list(df1_ids + 2, 3, AMT_ST_UNTESTABLE, "DF1 track 81 not verified before writing");
        seek(entry);
        drv_motor_off(1);
        return;
    }
    words = build_track(WR_TRACK);
    nominal = us_to_cc(words * 32UL);
    /* A host drive faster than a real one (run parameter floppy, CONTRACT
     * section 4) sends the words that much faster; turbo copies them at once,
     * so only "DSKBLK came" is left to check. */
    speed = amt_par.floppy ? amt_par.floppy : 100;
    if (speed != AMT_FLOPPY_TURBO)
        nominal = (ULONG)(((unsigned long long)nominal * 100) / speed);
    dsk_dma(wbuf, (UWORD)words, 1, ADK_WRITE, SYNC, 1, PA_MS(800), &r);
    ok = r.blk == 1 && (speed == AMT_FLOPPY_TURBO ||
                        (r.t_blk >= nominal - nominal / 10 && r.t_blk <= nominal + nominal / 10));
    amt_case_open("df1_write_dskblk", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    json_obj("in");
    json_int("words", (long)words);
    json_hexn("adkcon", ADK_WRITE, 4);
    if (speed == AMT_FLOPPY_TURBO)
        json_str("drive_speed", "turbo");
    else
        json_int("drive_speed_pct", (long)speed);
    json_str("bounds", speed == AMT_FLOPPY_TURBO ? "turbo drive: DSKBLK only, no time bound"
                                                : "nominal (words*16 cells*2 us *100/drive speed) +-10%");
    json_obj_end();
    json_obj("timing");
    j_dres(&r);
    json_int("nominal_cck", (long)nominal);
    json_obj_end();
    if (!ok) {
        json_obj("expect");
        json_int("dskblk", 1);
        json_int("t_dskblk_cck_lo", (long)(nominal - nominal / 10));
        json_int("t_dskblk_cck_hi", (long)(nominal + nominal / 10));
        json_obj_end();
        json_obj("got");
        json_int("dskblk", r.blk);
        json_str("t_dskblk", !r.blk ? "none" : r.t_blk < nominal - nominal / 10 ? "early"
                              : r.t_blk > nominal + nominal / 10 ? "late" : "in_range");
        json_obj("timing");
        json_int("t_dskblk_cck", (long)r.t_blk);
        json_obj_end();
        json_obj_end();
        json_str("why", "HRM ch.8: a write DMA (DSKLEN bit 14, written twice) sends the words at "
                 "2 us per MFM cell (ADKCON FAST) and raises DSKBLK when the last word has been "
                 "fetched: words*32 us after the start");
    }
    amt_case_close();
    ok = (r.bytr_or & 0x2000) != 0;
    amt_case_open("dskbytr_write", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_int("diskwrite_seen", 1);
        json_obj_end();
        json_obj("got");
        json_hexn("dskbytr_or", r.bytr_or, 4);
        json_obj_end();
        json_str("why", "HRM ch.8 DSKBYTR bit 13 DISKWRITE mirrors DSKLEN bit 14 (write) during a "
                 "write DMA");
    }
    amt_case_close();

    /* read back */
    read_track(WR_TRACK, &tr, &r);
    for (i = 0; i < 11 && bad_sec < 0; i++) {
        int k;
        if (!(tr.good & (1u << i))) {
            bad_sec = i;
            break;
        }
        pat_block(pat, first_lba + i);
        for (k = 0; k < 128; k++)
            if (pat[k] != secdata[i][k]) {
                bad_sec = i;
                bad_off = k * 4;
                break;
            }
    }
    if (bad_sec < 0 && !tr.bad_fields) {
        amt_case_open("df1_write_readback", AMT_ST_PASS);
        json_obj("timing");
        j_tres_timing(&tr);
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open("df1_write_readback", AMT_ST_FAIL);
        json_obj("in");
        json_int("track", WR_TRACK);
        json_int("first_lba", (long)first_lba);
        json_int("test_id", TEST_ID);
        json_int("seed", (long)amt_par.seed);
        json_obj_end();
        json_obj("expect");
        json_hexn("good", 0x7ff, 4);
        json_str("data", "CONTRACT section 11 test block per LBA");
        json_obj_end();
        json_obj("got");
        j_tres("track", &tr);
        json_int("bad_sector", bad_sec);
        json_int("bad_offset", bad_off);
        if (bad_off >= 0) {
            json_hex("raw", secdata[bad_sec][bad_off / 4]);
            json_hex("written", pat[bad_off / 4]);
        }
        json_obj("timing");
        j_tres_timing(&tr);
        json_obj_end();
        json_obj_end();
        json_str("why", "11 AmigaDOS sectors written to DF1 track 81 with a raw MFM write DMA must "
                 "read back (word-synced raw read) with good checksums and the same data");
        amt_case_close();
    }
    head_check("df1_head_restored", entry);
    drv_motor_off(1);
    if (rs_disp == 0)
        rs_drv = -1;
}

/* ------------------------------------------------------------ entry */
void pa_disk_tests(void)
{
    ULONG id[4];
    UBYTE ddrb = pa_cia_rd(CIAB, C_DDRB);
    int d;
    rbuf = (UWORD *)(pa_chip + PA_RDBUF_OFF);
    wbuf = (UWORD *)(pa_chip + PA_WRBUF_OFF);
    prb0 = prb = pa_cia_rd(CIAB, C_PRB);
    if (ddrb != 0xff) {
        static const char *const all[] = {
            "drive_ids", "df0_motor_ready", "df0_seek_track0", "df0_track0_sensor",
            "df0_disk_change", "dsklen_single_write", "dsklen_double_write", "dsklen_exact_count",
            "wordsync_waits",
            "wordsync_off", "dskblk_irq", "dsksyn_irq", "dskbytr_read", "df0_raw_sectors",
            "df0_bootblock", "df0_payload_header", "df0_blob_crc", "df0_head_restored"
        };
        case_skip_list(all, 18, AMT_ST_UNTESTABLE, "CIA-B DDRB is not $FF (drive lines not outputs)");
        case_skip_list(df1_ids, 5, AMT_ST_UNTESTABLE, "CIA-B DDRB is not $FF (drive lines not outputs)");
        return;
    }
    /* the OS leaves STEP high and all drives deselected */
    rs_active = 1;
    prb_w((UBYTE)(prb | PRB_SELS | PRB_STEP));
    CREG(R_DSKLEN) = 0x4000;
    amt_irq_hook[1] = hook1;
    amt_irq_hook[5] = hook5;
    CREG(R_INTREQ) = INT_DSKBLK | INT_DSKSYN;
    CREG(R_INTENA) = 0x8000 | INT_DSKBLK | INT_DSKSYN;

    for (d = 0; d < 4; d++)
        id[d] = drv_id(d);
    amt_case_open("drive_ids", AMT_ST_PASS);
    json_obj("in");
    json_arr("id");
    for (d = 0; d < 4; d++)
        json_hex(NULL, id[d]);
    json_arr_end();
    json_str("note", "informational: $FFFFFFFF 3.5\" DD drive, $55555555 5.25\", $00000000 none "
             "(HRM ch.8); DF0 need not answer");
    json_obj_end();
    amt_case_close();

    df0_tests();
    df1_tests(id[1]);
}

/* Called by amt_run_paula after pa_disk_tests on every path, also when it
 * was aborted (unexpected exception, watchdog, amt_abort): disk DMA off,
 * the moved head stepped back to where it was found, all motors off, the
 * drives deselected, CIA-B PRB as found. */
void pa_disk_restore(void)
{
    int d;
    CREG(R_DSKLEN) = 0x4000;
    CREG(R_DSKLEN) = 0x4000;
    CREG(R_DMACON) = 0x0010;
    CREG(R_INTENA) = INT_DSKBLK | INT_DSKSYN;
    CREG(R_INTREQ) = INT_DSKBLK | INT_DSKSYN;
    CREG(R_DSKSYNC) = SYNC;
    amt_irq_hook[1] = 0;
    amt_irq_hook[5] = 0;
    if (!rs_active)
        return;
    if (!(prb & PRB_STEP)) {
        prb_w(prb | PRB_STEP);          /* aborted inside a step pulse */
        cur_cyl += (prb & PRB_DIR) ? -1 : 1;
        rs_disp += (prb & PRB_DIR) ? -1 : 1;
        pa_wait_cc(PA_MS(4));
    }
    if (rs_drv >= 0 && rs_known && rs_disp != 0) {
        drv_select(rs_drv, 1);
        while (rs_disp < 0)
            step1(0);
        while (rs_disp > 0)
            step1(1);
        settle();
    }
    rs_drv = -1;
    for (d = 0; d < 4; d++)
        drv_motor_off(d);
    drv_deselect();
    prb_w(prb0);
    rs_active = 0;
}
