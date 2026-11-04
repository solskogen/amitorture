/* memory (module 21): every CPU access path to every kind of memory.
 *
 * Per memory region (ExecBase->MemList; blocks allocated by memory_os,
 * module 25, see memory.h):
 *   <kind>_rw_width   byte/word/long writes at every offset of a 16-byte
 *                     window, each read back with every width at every
 *                     offset (odd word/long offsets only on 68020+: on the
 *                     68000/010 they are address errors, exceptions module)
 *   <kind>_cross64k   the same with the window centred on a 64 KB boundary
 *   <kind>_cross32k   ... on an odd 32 KB boundary, <kind>_cross16m on a
 *                     16 MB boundary (if the region spans one); plus a
 *                     MOVEM.L burst across the boundary and guard bytes
 *   <kind>_patterns   address/inverse/walking-one/checkerboard/byte-ramp
 *                     patterns over the 2 KB block (caches pushed between
 *                     write and read)
 *   <kind>_movem      MOVEM.L/.W stores and loads (aligned, +2, +1/+3 on 020+)
 *   <kind>_move16     MOVE16 (Ax)+,(Ay)+ (68040/68060; not on chip-bus RAM)
 *   <kind>_tas        TAS (not on chip or slow RAM: the Amiga chip bus has no
 *                     indivisible read-modify-write cycle, HRM)
 *   <kind>_cas        CAS.B/.W/.L, CAS2.L (020+; CAS2 not on the 68060)
 *   perf records read_/write_/movem_<kind>
 * and once: rom_read (Kickstart ROM read by bytes and by longs gives the
 * same CRC-32), rom_write (writes to the ROM do not change it), custom_readback
 * (DMACONR/INTENAR show what was written to DMACON/INTENA; long access),
 * custom_mirrors (skip: the HRM defines no custom register mirrors).
 * Expected values come from the 680x0 big-endian byte order and the PRM
 * definitions of the instructions; nothing is taken from an emulator.
 */
#include "mod.h"
#include "memory.h"

AMT_MODULE(memory, 21, AMT_PH_BARE, 0, 0, 0);

void *memcpy(void *, const void *, unsigned long);

/* memory_asm.s */
ULONG mem_rd(ULONG addr, ULONG size);
void  mem_wr(ULONG addr, ULONG size, ULONG v);
void  mem_movem_st(ULONG addr, const ULONG *v14, ULONG word);
void  mem_movem_ld(ULONG addr, ULONG *out14, ULONG word);
ULONG mem_tas(ULONG addr, ULONG ccr);
void  mem_bench(ULONG addr, ULONG passes, ULONG mode);
ULONG mem_crc(ULONG crc, ULONG addr, ULONG len, ULONG longs, const ULONG *tab);
/* memory_020.s */
ULONG mem_cas(ULONG addr, ULONG size, ULONG *dc, ULONG du, ULONG ccr);
ULONG mem_cas2(ULONG *addr2, ULONG *dc2, const ULONG *du2, ULONG size, ULONG ccr);
void  mem_cacr_or(ULONG bits);
/* memory_040.s */
void  mem_move16(ULONG src, ULONG dst, ULONG lines);
void  mem_cpusha_dc(void);
void  mem_cinvl_dc(ULONG a);
extern ULONG mem_m16_a0, mem_m16_a1;

static int c020;            /* misaligned word/long allowed */
static int fam;             /* 0: 000/010, 2: 020, 3: 030, 4: 040/060 */
static int is060;
static char idbuf[40];

static const char *cid(const char *kind, const char *test)
{
    char *q = idbuf;
    while (*kind)
        *q++ = *kind++;
    *q++ = '_';
    while (*test)
        *q++ = *test++;
    *q = 0;
    return idbuf;
}

/* make writes visible in memory and drop cached copies (supervisor) */
static void dc_push(void)
{
    if (fam == 3)
        mem_cacr_or(0x0800);         /* 68030 CD: clear the (write-through) data cache */
    else if (fam == 4)
        mem_cpusha_dc();
}

static ULONG pat(int i)
{
    ULONG x = 0x9e3779b9UL * (ULONG)(i + 1);
    return x ^ (x >> 15) ^ 0x5a000000UL;
}

/* ------------------------------------------------------------ widths */
struct rwfail {
    UBYTE wsz, wo, rsz, ro;
    ULONG wv, exp, got;
    int guard;               /* guard byte offset that changed, or -1 */
    UBYTE eimg[16], gimg[16];/* the 16-byte window: expected, read back as bytes */
};
static const struct mem_reg *creg;    /* region under test (for records) */

static void hexbytes(const char *key, const UBYTE *b, int n)
{
    static const char hx[] = "0123456789abcdef";
    char t[80];
    int i;
    for (i = 0; i < n && i < 39; i++) {
        t[2 * i] = hx[b[i] >> 4];
        t[2 * i + 1] = hx[b[i] & 15];
    }
    t[2 * i] = 0;
    json_str(key, t);
}

static void region_obj(void)
{
    if (!creg)
        return;
    json_obj("region");
    json_str("kind", creg->kind);
    json_hex("lower", creg->lower);
    json_hex("upper", creg->upper);
    json_hexn("attr", creg->attr, 4);
    json_obj_end();
}

static ULONG img_get(const UBYTE *img, int o, int sz)
{
    ULONG v = 0;
    while (sz--)
        v = (v << 8) | img[o++];
    return v;
}

static int aligned_ok(int sz, int off)
{
    return sz == 1 || c020 || !(off & 1);
}

/* window [base, base+16) with 16 guard bytes on each side */
static int rw_window(ULONG base, struct rwfail *f)
{
    UBYTE img[16];
    int wsz, wo, rsz, ro, i, inv;
    for (i = -16; i < 32; i += 4)
        mem_wr(base + i, 4, 0xc3c3c3c3UL ^ (ULONG)i);
    for (inv = 0; inv < 2; inv++)
        for (wsz = 1; wsz <= 4; wsz <<= 1)
            for (wo = 0; wo + wsz <= 16; wo++) {
                ULONG v;
                if (!aligned_ok(wsz, wo))
                    continue;
                for (i = 0; i < 16; i += 4) {
                    ULONG b = 0x01234567UL + 0x11111111UL * (ULONG)i;
                    mem_wr(base + i, 4, b);
                    img[i] = (UBYTE)(b >> 24);
                    img[i + 1] = (UBYTE)(b >> 16);
                    img[i + 2] = (UBYTE)(b >> 8);
                    img[i + 3] = (UBYTE)b;
                }
                v = pat(wsz * 16 + wo);
                if (inv)
                    v = ~v;
                if (wsz < 4)
                    v &= (1UL << (8 * wsz)) - 1;
                mem_wr(base + wo, wsz, v);
                /* push the write to memory and drop the cached line, so
                 * the reads below see the bank's byte lanes, not a
                 * copyback cache (030 CD, 040/060 CPUSHA DC) */
                dc_push();
                for (i = 0; i < wsz; i++)
                    img[wo + i] = (UBYTE)(v >> (8 * (wsz - 1 - i)));
                for (rsz = 1; rsz <= 4; rsz <<= 1)
                    for (ro = 0; ro + rsz <= 16; ro++) {
                        ULONG e, g;
                        if (!aligned_ok(rsz, ro))
                            continue;
                        e = img_get(img, ro, rsz);
                        g = mem_rd(base + ro, rsz);
                        if (e != g) {
                            f->wsz = wsz; f->wo = wo; f->rsz = rsz; f->ro = ro;
                            f->wv = v; f->exp = e; f->got = g; f->guard = -1;
                            for (i = 0; i < 16; i++) {
                                f->eimg[i] = img[i];
                                f->gimg[i] = (UBYTE)mem_rd(base + (ULONG)i, 1);
                            }
                            return 0;
                        }
                    }
            }
    for (i = -16; i < 32; i += 4) {
        ULONG e = 0xc3c3c3c3UL ^ (ULONG)i, g;
        if (i >= 0 && i < 16)
            continue;
        g = mem_rd(base + i, 4);
        if (g != e) {
            f->wsz = f->wo = f->rsz = f->ro = 0;
            f->wv = 0; f->exp = e; f->got = g; f->guard = i;
            for (wo = 0; wo < 16; wo++) {
                f->eimg[wo] = img[wo];
                f->gimg[wo] = (UBYTE)mem_rd(base + (ULONG)wo, 1);
            }
            return 0;
        }
    }
    return 1;
}

/* open a failure record with the region and CPU context */
static void fail_open(const char *id)
{
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("ctx");
    region_obj();
    json_str("cpu", amt_probe_result.cpu);
    json_hexn("cacr", amt_probe_result.cacr, 8);
    json_obj_end();
}

static void rw_report(const char *id, ULONG base, const struct rwfail *f, const char *what)
{
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("in");
    json_hex("window", base);
    json_str("what", what);
    if (f->guard < 0) {
        json_int("write_size", f->wsz);
        json_int("write_off", f->wo);
        json_hex("write_value", f->wv);
        json_int("read_size", f->rsz);
        json_int("read_off", f->ro);
    } else {
        json_int("guard_off", f->guard);
    }
    json_obj_end();
    json_obj("expect");
    json_hex("value", f->exp);
    hexbytes("window_bytes", f->eimg, 16);
    json_obj_end();
    json_obj("got");
    json_hex("value", f->got);
    hexbytes("window_bytes", f->gimg, 16);
    json_obj_end();
    json_obj("ctx");
    region_obj();
    json_str("cpu", amt_probe_result.cpu);
    json_hexn("cacr", amt_probe_result.cacr, 8);
    json_str("method", "window filled with longs $01234567+$11111111*i, then one write of write_size at "
                       "write_off; every read of every size at every allowed offset compared; got.window_bytes "
                       "read back with MOVE.B right after the mismatch");
    json_obj_end();
    json_str("why", f->guard < 0
             ? "a read after a write of another width/offset does not give the big-endian bytes written "
               "(PRM 2.2: byte n of a word/long at address a is at a+n, most significant first)"
             : "a write changed memory outside the bytes it addresses");
    amt_case_close();
}

static void t_rw(const char *kind, ULONG base, const char *test, const char *what)
{
    struct rwfail f;
    const char *id = cid(kind, test);
    if (rw_window(base, &f))
        amt_case(id, AMT_ST_PASS, 0);
    else
        rw_report(id, base, &f, what);
}

/* window centred on the boundary, plus a MOVEM.L burst across it */
static void t_cross(const char *kind, ULONG blk, const char *test)
{
    struct rwfail f;
    ULONG b = blk + MEM_HALF;
    ULONG v[14], o[14];
    const char *id = cid(kind, test);
    int i, off;
    if (!rw_window(b - 8, &f)) {
        rw_report(id, b - 8, &f, "16-byte window across the boundary");
        return;
    }
    /* MOVEM.L of 14 registers from b-28 (+2 or +1 on 020+): 56 bytes across b */
    for (off = 0; off < 3; off++) {
        ULONG a = b - 28 + (ULONG)(off == 0 ? 0 : off == 1 ? 2 : 1);
        if (off == 2 && !c020)
            break;
        for (i = 0; i < 14; i++)
            v[i] = pat(100 + i + off * 16);
        mem_movem_st(a, v, 0);
        dc_push();
        for (i = 0; i < 56; i++) {
            ULONG e = (v[i >> 2] >> (8 * (3 - (i & 3)))) & 0xff, g = mem_rd(a + i, 1);
            if (e != g) {
                fail_open(id);
                json_obj("in");
                json_hex("boundary", b);
                json_hex("movem_addr", a);
                json_int("byte", i);
                json_obj_end();
                json_obj("expect");
                json_hexn("value", e, 2);
                json_obj_end();
                json_obj("got");
                json_hexn("value", g, 2);
                json_obj_end();
                json_str("why", "MOVEM.L store across the bank boundary: byte read back differs");
                amt_case_close();
                return;
            }
        }
        mem_movem_ld(a, o, 0);
        for (i = 0; i < 14; i++)
            if (o[i] != v[i]) {
                fail_open(id);
                json_obj("in");
                json_hex("boundary", b);
                json_hex("movem_addr", a);
                json_int("reg", i);
                json_obj_end();
                json_obj("expect");
                json_hex("value", v[i]);
                json_obj_end();
                json_obj("got");
                json_hex("value", o[i]);
                json_obj_end();
                json_str("why", "MOVEM.L load across the bank boundary returned a wrong register (order d0-d7/a0-a5)");
                amt_case_close();
                return;
            }
    }
    amt_case(id, AMT_ST_PASS, 0);
}

/* ------------------------------------------------------------ patterns */
static ULONG pval(int p, ULONG a, int i)
{
    switch (p) {
    case 0: return a;
    case 1: return ~a;
    case 2: return 1UL << (i & 31);
    case 3: return (i & 1) ? 0xaaaaaaaaUL : 0x55555555UL;
    default: return ((ULONG)(4 * i) << 24 | (ULONG)(4 * i + 1) << 16 | (ULONG)(4 * i + 2) << 8 | (ULONG)(4 * i + 3))
                     & 0xffffffffUL;
    }
}

static void t_patterns(const char *kind, ULONG blk)
{
    static const char *const pn[5] = { "address", "inverse_address", "walking_one", "checkerboard", "byte_ramp" };
    const char *id = cid(kind, "patterns");
    int p, i;
    for (p = 0; p < 5; p++) {
        for (i = 0; i < MEM_BLK / 4; i++) {
            ULONG a = blk + 4 * (ULONG)i;
            if (p == 4) {
                ULONG v = pval(p, a, i);
                mem_wr(a, 1, v >> 24);
                mem_wr(a + 1, 1, (v >> 16) & 0xff);
                mem_wr(a + 2, 1, (v >> 8) & 0xff);
                mem_wr(a + 3, 1, v & 0xff);
            } else {
                mem_wr(a, 4, pval(p, a, i));
            }
        }
        dc_push();
        for (i = 0; i < MEM_BLK / 4; i++) {
            ULONG a = blk + 4 * (ULONG)i, e = pval(p, a, i), g = mem_rd(a, 4);
            if (e != g) {
                fail_open(id);
                json_obj("in");
                json_str("pattern", pn[p]);
                json_hex("addr", a);
                json_str("written_as", p == 4 ? "bytes" : "longs");
                json_hex("block", blk);
                json_obj_end();
                json_obj("expect");
                json_hex("value", e);
                json_obj_end();
                json_obj("got");
                json_hex("value", g);
                json_obj_end();
                json_str("why", "memory does not hold the pattern written over the 2 KB block (read as longs)");
                amt_case_close();
                return;
            }
        }
    }
    amt_case(id, AMT_ST_PASS, 0);
}

/* ------------------------------------------------------------ MOVEM */
static void t_movem(const char *kind, ULONG blk)
{
    static const UBYTE offs[4] = { 0, 2, 1, 3 };
    const char *id = cid(kind, "movem");
    ULONG v[14], o[14];
    int k, w, i;
    for (k = 0; k < (c020 ? 4 : 2); k++)
        for (w = 0; w < 2; w++) {
            ULONG a = blk + 256 + offs[k];
            int sz = w ? 2 : 4;
            for (i = 0; i < 70; i++)
                mem_wr(a - 4 + (ULONG)i, 1, 0xee);
            for (i = 0; i < 14; i++)
                v[i] = pat(200 + i + 16 * k + 64 * w) ^ (i & 1 ? 0x00008000UL : 0);
            mem_movem_st(a, v, (ULONG)w);
            dc_push();
            for (i = -4; i < 14 * sz + 4; i++) {
                ULONG e, g = mem_rd(a + (ULONG)i, 1);
                if (i < 0 || i >= 14 * sz)
                    e = 0xee;
                else
                    e = (v[i / sz] >> (8 * (sz - 1 - i % sz))) & 0xff;
                if (e != g) {
                    fail_open(id);
                    json_obj("in");
                    json_str("op", w ? "movem.w d0-d7/a0-a5,(a6)" : "movem.l d0-d7/a0-a5,(a6)");
                    json_hex("a6", a);
                    json_int("byte", i);
                    json_obj_end();
                    json_obj("expect");
                    json_hexn("value", e, 2);
                    json_obj_end();
                    json_obj("got");
                    json_hexn("value", g, 2);
                    json_obj_end();
                    json_str("why", "PRM MOVEM register-to-memory: registers stored in the order d0-d7/a0-a7 at "
                                    "ascending addresses, word form stores the low words; nothing else written");
                    amt_case_close();
                    return;
                }
            }
            mem_movem_ld(a, o, (ULONG)w);
            for (i = 0; i < 14; i++) {
                ULONG e = w ? (ULONG)(LONG)(WORD)(v[i] & 0xffff) : v[i];
                if (o[i] != e) {
                    fail_open(id);
                    json_obj("in");
                    json_str("op", w ? "movem.w (a6),d0-d7/a0-a5" : "movem.l (a6),d0-d7/a0-a5");
                    json_hex("a6", a);
                    json_int("reg", i);
                    json_obj_end();
                    json_obj("expect");
                    json_hex("value", e);
                    json_obj_end();
                    json_obj("got");
                    json_hex("value", o[i]);
                    json_obj_end();
                    json_str("why", "PRM MOVEM memory-to-register: loaded in the order d0-d7/a0-a7; word loads are "
                                    "sign-extended to 32 bits (data registers too)");
                    amt_case_close();
                    return;
                }
            }
        }
    amt_case(id, AMT_ST_PASS, 0);
}

#define PCMCIA_WHY "memory at $600000-$9FFFFF without an autoconfig board (Gayle PCMCIA card memory): " \
                   "an indivisible RMW cycle or a MOVE16 line burst is not guaranteed by the card interface"

/* ------------------------------------------------------------ MOVE16 */
static void t_move16(const char *kind, ULONG blk, int chipbus)
{
    const char *id = cid(kind, "move16");
    ULONG src = blk + 512, dst = blk + 1024, i;
    int k;
    if (AMT_CPU() < AMT_CPU_68EC040) {
        amt_case(id, AMT_ST_SKIP, "MOVE16 is a 68040/68060 instruction");
        return;
    }
    if (chipbus) {
        amt_case(id, AMT_ST_SKIP, chipbus == 2 ? PCMCIA_WHY : "MOVE16 burst transfers are not tested on chip-bus memory");
        return;
    }
    for (k = 0; k < 2; k++) {
        /* k=1: registers 4 and 8 bytes past the line start: MC68040UM MOVE16:
         * the low four address bits are ignored, the registers still step by 16 */
        ULONG s0 = src + (k ? 4 : 0), d0 = dst + (k ? 8 : 0);
        for (i = 0; i < 64 + 16; i += 4)
            mem_wr(src + i, 4, pat((int)(300 + i + 64 * (ULONG)k)));
        for (i = 0; i < 16 + 64 + 16; i += 4)
            mem_wr(dst - 16 + i, 4, 0x77777777UL);
        dc_push();
        mem_move16(s0, d0, 4);
        dc_push();
        for (i = 0; i < 64 + 16; i += 4) {
            ULONG e = i < 64 ? pat((int)(300 + i + 64 * (ULONG)k)) : 0x77777777UL;
            ULONG g = mem_rd(dst + i, 4);
            if (g != e || (i < 16 && mem_rd(dst - 16 + i, 4) != 0x77777777UL) ||
                mem_m16_a0 != s0 + 64 || mem_m16_a1 != d0 + 64) {
                fail_open(id);
                json_obj("in");
                json_str("op", "4 x move16 (a0)+,(a1)+");
                json_hex("a0", s0);
                json_hex("a1", d0);
                json_obj_end();
                json_obj("expect");
                json_hex("off", i);
                json_hex("value", e);
                json_hex("a0", s0 + 64);
                json_hex("a1", d0 + 64);
                json_obj_end();
                json_obj("got");
                json_hex("off", i);
                json_hex("value", g);
                json_hex("a0", mem_m16_a0);
                json_hex("a1", mem_m16_a1);
                json_obj_end();
                json_str("why", "MC68040UM MOVE16: copies the aligned 16-byte line (address bits 3-0 ignored) and "
                                "post-increments both registers by 16");
                amt_case_close();
                return;
            }
        }
    }
    amt_case(id, AMT_ST_PASS, 0);
}

/* ------------------------------------------------------------ RMW */
static void t_tas(const char *kind, ULONG blk, int chipbus)
{
    static const UBYTE in[4] = { 0x00, 0x80, 0x7f, 0xff };
    const char *id = cid(kind, "tas");
    int i, c;
    if (chipbus) {
        amt_case(id, AMT_ST_SKIP, chipbus == 2 ? PCMCIA_WHY :
                                  "chip-bus RAM: the Amiga chip bus has no indivisible read-modify-write "
                                  "cycle, TAS must not be used there (HRM)");
        return;
    }
    for (c = 0; c < 2; c++)
        for (i = 0; i < 4; i++) {
            ULONG a = blk + 64 + (ULONG)i, ccr_in = c ? 0x1f : 0x00, r, m;
            ULONG e_ccr = (ccr_in & 0x10) | (in[i] & 0x80 ? 0x08 : 0) | (in[i] == 0 ? 0x04 : 0);
            mem_wr(a, 1, in[i]);
            r = mem_tas(a, ccr_in);
            m = mem_rd(a, 1);
            if (r != e_ccr || m != (ULONG)(in[i] | 0x80)) {
                fail_open(id);
                json_obj("in");
                json_hexn("mem", in[i], 2);
                json_hexn("ccr", ccr_in, 2);
                json_hex("addr", a);
                json_obj_end();
                json_obj("expect");
                json_hexn("mem", in[i] | 0x80, 2);
                json_hexn("ccr", e_ccr, 2);
                json_obj_end();
                json_obj("got");
                json_hexn("mem", m, 2);
                json_hexn("ccr", r, 2);
                json_obj_end();
                json_str("why", "PRM TAS: N,Z from the byte before, V=C=0, X unchanged; bit 7 of the byte set");
                amt_case_close();
                return;
            }
        }
    amt_case(id, AMT_ST_PASS, 0);
}

struct casv {
    UBYTE size, ccr_in, e_ccr, pad;
    ULONG mem, dc, du, e_mem, e_dc;
};
/* expectations worked out from PRM CAS: compare <ea> - Dc (CMP flags, X
 * unchanged); equal: Du -> <ea>; else <ea> -> Dc (low byte/word only) */
static const struct casv casv[] = {
    { 4, 0x10, 0x14, 0, 0x12345678UL, 0x12345678UL, 0xcafebabeUL, 0xcafebabeUL, 0x12345678UL },
    { 4, 0x10, 0x19, 0, 0x12345678UL, 0x12345679UL, 0xcafebabeUL, 0x12345678UL, 0x12345678UL },
    { 2, 0x00, 0x02, 0, 0x8000UL, 0xaaaa0001UL, 0x0000beefUL, 0x8000UL, 0xaaaa8000UL },
    { 2, 0x00, 0x04, 0, 0x8000UL, 0x55558000UL, 0x1234beefUL, 0xbeefUL, 0x55558000UL },
    { 1, 0x00, 0x09, 0, 0x00UL, 0xffffff01UL, 0x80UL, 0x00UL, 0xffffff00UL },
    { 1, 0x1f, 0x14, 0, 0x7fUL, 0x0000007fUL, 0xffffff80UL, 0x80UL, 0x0000007fUL },
};

static void cas_fail(const char *id, const char *op, ULONG a, const struct casv *c, ULONG mem, ULONG dc, ULONG ccr)
{
    fail_open(id);
    json_obj("in");
    json_str("op", op);
    json_hex("addr", a);
    json_int("size", c->size);
    json_hex("mem", c->mem);
    json_hex("dc", c->dc);
    json_hex("du", c->du);
    json_hexn("ccr", c->ccr_in, 2);
    json_obj_end();
    json_obj("expect");
    json_hex("mem", c->e_mem);
    json_hex("dc", c->e_dc);
    json_hexn("ccr", c->e_ccr, 2);
    json_obj_end();
    json_obj("got");
    json_hex("mem", mem);
    json_hex("dc", dc);
    json_hexn("ccr", ccr, 2);
    json_obj_end();
    json_str("why", "PRM CAS/CAS2: flags of <ea> - Dc as CMP (X unchanged); equal: Du is written to <ea>, "
                    "else <ea> is loaded into Dc");
    amt_case_close();
}

static void t_cas(const char *kind, ULONG blk, int chipbus)
{
    const char *id = cid(kind, "cas");
    unsigned i;
    if (AMT_CPU() < AMT_CPU_68EC020) {
        amt_case(id, AMT_ST_SKIP, "CAS/CAS2 are 68020+ instructions");
        return;
    }
    if (chipbus) {
        amt_case(id, AMT_ST_SKIP, chipbus == 2 ? PCMCIA_WHY :
                                  "chip-bus RAM: no indivisible read-modify-write cycle on the Amiga chip bus "
                                  "(HRM); CAS must not be used there");
        return;
    }
    for (i = 0; i < sizeof casv / sizeof casv[0]; i++) {
        const struct casv *c = &casv[i];
        ULONG a = blk + 128, dc = c->dc, r, m;
        mem_wr(a, c->size, c->mem);
        r = mem_cas(a, c->size, &dc, c->du, c->ccr_in);
        m = mem_rd(a, c->size);
        if (r != c->e_ccr || m != c->e_mem || dc != c->e_dc) {
            cas_fail(id, "cas", a, c, m, dc, r);
            return;
        }
    }
    if (!is060) {
        /* CAS2.L: both equal -> both written; second differs -> both loaded */
        ULONG a2[2], dc2[2], du2[2], r, m0, m1;
        int k;
        for (k = 0; k < 2; k++) {
            a2[0] = blk + 160;
            a2[1] = blk + 200;
            mem_wr(a2[0], 4, 0x11111111UL);
            mem_wr(a2[1], 4, 0x22222222UL);
            dc2[0] = 0x11111111UL;
            dc2[1] = k ? 0x22222223UL : 0x22222222UL;
            du2[0] = 0xaaaaaaaaUL;
            du2[1] = 0xbbbbbbbbUL;
            r = mem_cas2(a2, dc2, du2, 4, 0x10);
            m0 = mem_rd(a2[0], 4);
            m1 = mem_rd(a2[1], 4);
            if (k == 0 ? (r != 0x14 || m0 != 0xaaaaaaaaUL || m1 != 0xbbbbbbbbUL ||
                          dc2[0] != 0x11111111UL || dc2[1] != 0x22222222UL)
                       : (r != 0x19 || m0 != 0x11111111UL || m1 != 0x22222222UL ||
                          dc2[0] != 0x11111111UL || dc2[1] != 0x22222222UL)) {
                fail_open(id);
                json_obj("in");
                json_str("op", "cas2.l d0:d1,d2:d3,(a0):(a1)");
                json_hex("mem1", 0x11111111UL);
                json_hex("mem2", 0x22222222UL);
                json_hex("dc1", 0x11111111UL);
                json_hex("dc2", k ? 0x22222223UL : 0x22222222UL);
                json_hex("du1", 0xaaaaaaaaUL);
                json_hex("du2", 0xbbbbbbbbUL);
                json_obj_end();
                json_obj("expect");
                json_hex("mem1", k ? 0x11111111UL : 0xaaaaaaaaUL);
                json_hex("mem2", k ? 0x22222222UL : 0xbbbbbbbbUL);
                json_hex("dc1", 0x11111111UL);
                json_hex("dc2", 0x22222222UL);
                json_hexn("ccr", k ? 0x19 : 0x14, 2);
                json_obj_end();
                json_obj("got");
                json_hex("mem1", m0);
                json_hex("mem2", m1);
                json_hex("dc1", dc2[0]);
                json_hex("dc2", dc2[1]);
                json_hexn("ccr", r, 2);
                json_obj_end();
                json_str("why", "PRM CAS2: both operands equal -> Du1/Du2 written; otherwise both memory operands "
                                "loaded into Dc1/Dc2; flags from the comparison that decided (X unchanged)");
                amt_case_close();
                return;
            }
        }
    }
    amt_case(id, AMT_ST_PASS, 0);
}

/* ------------------------------------------------------------ perf */
static void t_perf(const char *kind, ULONG blk)
{
    static const char *const nm[3] = { "read_", "write_", "movem_" };
    ULONG passes = AMT_QUICK() ? 8 : 32;
    int m;
    for (m = 0; m < 3; m++) {
        ULONG t0, t1;
        char name[32], *q = name;
        const char *s = nm[m], *k = kind;
        while (*s)
            *q++ = *s++;
        while (*k)
            *q++ = *k++;
        *q = 0;
        t0 = amt_ticks();
        mem_bench(blk, passes, (ULONG)m);
        t1 = amt_ticks();
        amt_perf(name, passes * (m == 2 ? 1024 : 2048), t1 - t0);
    }
}

/* ------------------------------------------------------------ ROM */
static ULONG rom_size(void)
{
    ULONG b = amt_probe_result.rom_base;
    return (b & 0x7ffffUL) == 0x40000UL ? 0x40000UL : 0x80000UL;
}

static ULONG crctab[256];

static void crc_init(void)
{
    ULONG i, c;
    int k;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xedb88320UL : c >> 1;
        crctab[i] = c;
    }
}

static void t_rom_read(void)
{
    ULONG base = amt_probe_result.rom_base, size = rom_size(), n, i;
    ULONG cb = 0, cl = 0, sum = 0, t0, t1, chunk = 0x4000;
    if (base < 0xe00000UL || base >= 0x1000000UL) {
        amt_case("rom_read", AMT_ST_UNTESTABLE, "ROM base from the probe is not in $E00000-$FFFFFF");
        return;
    }
    crc_init();
    n = AMT_QUICK() ? 0x10000UL : size;
    t0 = amt_ticks();
    for (i = 0; i < n; i += chunk) {
        cb = mem_crc(cb, base + i, chunk, 0, crctab);
        cl = mem_crc(cl, base + i, chunk, 1, crctab);
        amt_watchdog_kick();
    }
    t1 = amt_ticks();
    amt_perf("rom_crc2", 2 * n, t1 - t0);
    if (n == size)
        for (i = 0; i < n; i += 4) {
            ULONG l = *(volatile ULONG *)(base + i), s2 = sum + l;
            if (s2 < sum)
                s2++;
            sum = s2;
        }
    if (cb == cl) {
        amt_case_open("rom_read", AMT_ST_PASS);
        json_obj("ctx");
        json_hex("base", base);
        json_hex("bytes", n);
        json_hex("crc32", cl);
        if (n == size)
            json_hex("kick_sum", sum);
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open("rom_read", AMT_ST_FAIL);
        json_obj("in");
        json_hex("base", base);
        json_hex("bytes", n);
        json_obj_end();
        json_obj("expect");
        json_hex("crc32_bytes", cl);
        json_str("rule", "byte and long reads of the ROM give the same bytes");
        json_obj_end();
        json_obj("got");
        json_hex("crc32_bytes", cb);
        json_hex("crc32_longs", cl);
        json_obj_end();
        json_str("why", "reading the Kickstart ROM with MOVE.B and with MOVE.L gives different data (PRM 2.2 "
                        "big-endian byte order: they must agree)");
        amt_case_close();
    }
}

static void t_rom_write(void)
{
    ULONG a = amt_probe_result.rom_base + 0x100, r0, r1, sz;
    UWORD sr;
    int k;
    static const UBYTE sizes[3] = { 2, 4, 1 };
    if (a < 0xe00000UL || a >= 0x1000000UL) {
        amt_case("rom_write", AMT_ST_UNTESTABLE, "ROM base from the probe is not in $E00000-$FFFFFF");
        return;
    }
    sr = amt_get_sr();
    for (k = 0; k < 3; k++) {
        sz = sizes[k];
        amt_set_sr(0x2700);
        r0 = mem_rd(a, sz);
        mem_wr(a, sz, ~r0);
        if (fam == 3)
            mem_cacr_or(0x0800);
        else if (fam == 4)
            mem_cinvl_dc(a);
        r1 = mem_rd(a, sz);
        if (r1 != r0)
            mem_wr(a, sz, r0);          /* writable: put it back at once */
        amt_set_sr(sr);
        if (r1 != r0) {
            ULONG mask = sz == 4 ? 0xffffffffUL : (1UL << (8 * sz)) - 1;
            if (r1 == (~r0 & mask)) {
                amt_case("rom_write", AMT_ST_SKIP, "the Kickstart area is writable here (MAPROM / RAM "
                                                   "Kickstart): the write test does not apply (restored)");
                return;
            }
            amt_case_open("rom_write", AMT_ST_FAIL);
            json_obj("in");
            json_hex("addr", a);
            json_int("size", (long)sz);
            json_hex("wrote", ~r0 & mask);
            json_obj_end();
            json_obj("expect");
            json_hex("value", r0);
            json_obj_end();
            json_obj("got");
            json_hex("value", r1);
            json_obj_end();
            json_str("why", "a write to the Kickstart ROM changed what it reads back, to neither the old nor the "
                            "written value");
            amt_case_close();
            return;
        }
    }
    amt_case("rom_write", AMT_ST_PASS, 0);
}

/* ------------------------------------------------------------ custom */
static void t_custom(void)
{
    UWORD dmacon, intena, handed;
    ULONG l;
    /* The DMA state a module is handed depends on the status screen
     * (rt/screen.c: DMAEN|BPLEN|COPEN, or 0 with noscreen), so the case sets
     * a known state itself: clear every enable bit, read back, restore. */
    handed = CREG(R_DMACONR) & 0x07ff;
    CREG(R_DMACON) = 0x07ff;
    dmacon = CREG(R_DMACONR) & 0x07ff;
    CREG(R_DMACON) = (UWORD)(0x8000 | handed);
    intena = CREG(R_INTENAR);
    l = *(volatile ULONG *)(CUSTOM_BASE + R_INTENAR);
    if (dmacon == 0 && intena == 0x4020 && (l >> 16) == 0x4020)
        amt_case("custom_readback", AMT_ST_PASS, 0);
    else {
        amt_case_open("custom_readback", AMT_ST_FAIL);
        json_obj("in");
        json_str("written", "DMACON=$07ff (clear every enable bit, by this case); INTENA=$7fff then $c020 "
                            "(by the runtime)");
        json_hexn("dmacon_handed", handed, 4);
        json_obj_end();
        json_obj("expect");
        json_hexn("dmaconr_07ff", 0, 4);
        json_hexn("intenar", 0x4020, 4);
        json_hexn("long_01c_hi", 0x4020, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("dmaconr_07ff", dmacon, 4);
        json_hexn("intenar", intena, 4);
        json_hexn("long_01c_hi", l >> 16, 4);
        json_obj_end();
        json_str("why", "HRM: DMACONR/INTENAR read back the DMACON/INTENA bits (SET/CLR writes); a long read of "
                        "$DFF01C returns INTENAR in its upper word");
        amt_case_close();
    }
    amt_case("custom_mirrors", AMT_ST_SKIP, "the HRM defines no address mirrors of the custom chip registers; "
                                            "not tested");
}

/* ------------------------------------------------------------ main */
void amt_run_memory(void)
{
    LONG i;
    c020 = AMT_CPU() >= AMT_CPU_68EC020;
    is060 = AMT_CPU() >= AMT_CPU_68EC060;
    switch (AMT_CPU()) {
    case AMT_CPU_68000: case AMT_CPU_68010: fam = 0; break;
    case AMT_CPU_68EC020: case AMT_CPU_68020: fam = 2; break;
    case AMT_CPU_68EC030: case AMT_CPU_68030: fam = 3; break;
    default: fam = 4; break;
    }
    if (!mem_os_ran) {
        amt_case("regions", AMT_ST_UNTESTABLE, "memory_os (module 25) did not run: no test blocks");
    } else {
        for (i = 0; i < mem_nreg; i++) {
            const struct mem_reg *r = &mem_regs[i];
            ULONG main;
            creg = r;
            main = r->blk[MB_64K] ? r->blk[MB_64K] : r->blk[MB_PLAIN];
            if (!main)
                continue;           /* memory_os reported <kind>_alloc untestable */
            t_rw(r->kind, main + 64, "rw_width", "16-byte window inside the block");
            if (r->blk[MB_64K])
                t_cross(r->kind, r->blk[MB_64K], "cross64k");
            else
                amt_case(cid(r->kind, "cross64k"), AMT_ST_UNTESTABLE, "no free 64 KB boundary in this region");
            if (r->blk[MB_32K])
                t_cross(r->kind, r->blk[MB_32K], "cross32k");
            else
                amt_case(cid(r->kind, "cross32k"), AMT_ST_UNTESTABLE, "no free odd 32 KB boundary in this region");
            if (r->blk[MB_16M])
                t_cross(r->kind, r->blk[MB_16M], "cross16m");
            else if (((r->lower + MEM_HALF) | 0xffffffUL) + 1 + MEM_HALF <= r->upper &&
                     ((r->lower + MEM_HALF) | 0xffffffUL) + 1 > r->lower + MEM_HALF)
                amt_case(cid(r->kind, "cross16m"), AMT_ST_UNTESTABLE, "no free 16 MB boundary in this region");
            else
                amt_case(cid(r->kind, "cross16m"), AMT_ST_SKIP, "the region does not span a 16 MB boundary");
            t_patterns(r->kind, main);
            t_movem(r->kind, main);
            t_move16(r->kind, main, r->chipbus);
            t_tas(r->kind, main, r->chipbus);
            t_cas(r->kind, main, r->chipbus);
            t_perf(r->kind, main);
            amt_watchdog_kick();
        }
    }
    t_rom_read();
    t_rom_write();
    t_custom();
}
