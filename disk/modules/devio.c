/* devio: trackdisk/scsi.device block I/O on signed test-owned media
 * (CONTRACT.md sections 10 and 11; module id 11, OS phase).
 *
 * For every unit of trackdisk.device (0-3) and of every scsi-style device
 * (scsi.device, 2nd./3rd.scsi.device, uaehf.device; units 0-7, every node of
 * that name in the exec device list, so an A4091 next to the A4000 IDE is
 * reached too) this module:
 *   1. opens the unit; reads block 0 (always: that is how ownership is found;
 *      the data of an unowned unit is never reported or written);
 *   2. if block 0 carries the section 11 signature, reads the last block
 *      (TD_GETGEOMETRY, else SCSI READ CAPACITY / TD_GETNUMTRACKS) and
 *      accepts the unit ONLY if it is signed too, same UUID;
 *   3. on owned units: write/read/verify of 1, 2, 7, 16, 64, 255 blocks with
 *      16-byte-aligned and word-aligned (2 mod 4) buffers, CRC-checking every
 *      block and guard bytes around the buffer; odd (byte) buffers are skipped
 *      because the device documentation requires word-aligned io_Data
 *      (devices/scsidisk.h, trackdisk.doc); then four requests in flight
 *      (SendIO x4, completion by reply) for writes and reads;
 *   4. unowned units: skip "not test-owned", or with AMT_F_UNSAFE one
 *      read-only case for block 0.
 * Every request has a timeout (timer.device); a request that never comes back
 * even after AbortIO is reported as hung and all further I/O is stopped
 * (its IORequest and buffer are left alone, never freed).
 * Latencies (separate "timing" fields and perf records) come from
 * timer.device ReadEClock (V36+) or TR_GETSYSTIME (V34).
 *
 * TODO: the DF1 results-disk transport from the plan is not implemented here.
 */
#include <proto/exec.h>
#include <exec/memory.h>
#include <exec/devices.h>
#include <exec/execbase.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/timer.h>
#include <proto/timer.h>
#include "mod.h"

AMT_MODULE(devio, 11, AMT_PH_OS, 0, 0, 0);

void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);

struct Device *TimerBase;

#define BS        512
#define MAXBLK    255
#define NQ        4
#define ASYNC_BLK 16
#define GUARD     32
#define MAXLAT    48

/* ------------------------------------------------------------ small utils */
static char *scpy(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

static char idbuf[64];
static const char *cid(const char *key, ULONG unit, const char *suffix)
{
    char *p = scpy(idbuf, key);
    p = scpy(p, "_u");
    p = amt_utoa(p, unit);
    if (suffix) {
        *p++ = '_';
        p = scpy(p, suffix);
    }
    return idbuf;
}

/* ------------------------------------------------------------ ports, clock */
static struct MsgPort dport, tport, cport;
static struct timerequest treq, creq;
static int timer_ok, have_eclock;
static ULONG clk_hz;            /* ticks/s of clk_now(): E clock or 1e6 */

static int port_init(struct MsgPort *p)
{
    BYTE sig = AllocSignal(-1);
    if (sig < 0)
        return 0;
    memset(p, 0, sizeof *p);
    p->mp_Node.ln_Type = NT_MSGPORT;
    p->mp_Flags = PA_SIGNAL;
    p->mp_SigBit = sig;
    p->mp_SigTask = FindTask(0);
    p->mp_MsgList.lh_Head = (struct Node *)&p->mp_MsgList.lh_Tail;
    p->mp_MsgList.lh_Tail = 0;
    p->mp_MsgList.lh_TailPred = (struct Node *)&p->mp_MsgList.lh_Head;
    return 1;
}

static ULONG clk_now(void)
{
    if (have_eclock) {
        struct EClockVal ev;
        ReadEClock(&ev);
        return ev.ev_lo;
    }
    if (!timer_ok)
        return 0;
    creq.tr_node.io_Command = TR_GETSYSTIME;
    DoIO((struct IORequest *)&creq);
    return creq.tr_time.tv_secs * 1000000UL + creq.tr_time.tv_micro;
}

static ULONG to_us(ULONG dt)
{
    return (ULONG)(((unsigned long long)dt * 1000000ULL) / clk_hz);
}

static ULONG to_eticks(ULONG dt)
{
    ULONG ehz = amt_eclock_hz() / 5;
    if (have_eclock)
        return dt;
    return (ULONG)(((unsigned long long)dt * ehz) / 1000000ULL);
}

static void timer_open(void)
{
    port_init(&tport);
    port_init(&cport);
    memset(&treq, 0, sizeof treq);
    treq.tr_node.io_Message.mn_ReplyPort = &tport;
    treq.tr_node.io_Message.mn_Length = sizeof treq;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)&treq, 0) != 0)
        return;
    timer_ok = 1;
    creq = treq;
    creq.tr_node.io_Message.mn_ReplyPort = &cport;
    TimerBase = treq.tr_node.io_Device;
    if (TimerBase->dd_Library.lib_Version >= 36) {
        struct EClockVal ev;
        have_eclock = 1;
        clk_hz = ReadEClock(&ev);
        if (!clk_hz)
            have_eclock = 0;
    }
    if (!have_eclock)
        clk_hz = 1000000UL;
}

static void timer_close(void)
{
    if (timer_ok)
        CloseDevice((struct IORequest *)&treq);
    timer_ok = 0;
}

static int timer_pending;
static void timeout_start(ULONG secs)
{
    if (!timer_ok)
        return;
    treq.tr_node.io_Command = TR_ADDREQUEST;
    treq.tr_time.tv_secs = secs;
    treq.tr_time.tv_micro = 0;
    SendIO((struct IORequest *)&treq);
    timer_pending = 1;
}
static int timeout_fired(void)
{
    return timer_pending && CheckIO((struct IORequest *)&treq) != 0;
}
static void timeout_stop(void)
{
    if (!timer_pending)
        return;
    if (!CheckIO((struct IORequest *)&treq))
        AbortIO((struct IORequest *)&treq);
    WaitIO((struct IORequest *)&treq);
    timer_pending = 0;
}

/* ------------------------------------------------------------ requests */
struct rq {
    struct IOExtTD io;
    ULONG t0, t1;
    UBYTE state;               /* 0 idle, 1 pending, 2 done, 3 hung */
    UBYTE timed_out;
};
static struct rq rq[NQ];
static int hung;               /* a request never came back: stop all I/O */
static ULONG io_timeout;       /* seconds */

static ULONG lat_us[MAXLAT];
static int n_lat, n_timeouts;
static ULONG lat_sum;          /* clock ticks */

static void rq_prep(struct rq *r, UWORD cmd, ULONG off, ULONG len, APTR data)
{
    r->io.iotd_Req.io_Command = cmd;
    r->io.iotd_Req.io_Flags = 0;
    r->io.iotd_Req.io_Offset = off;
    r->io.iotd_Req.io_Length = len;
    r->io.iotd_Req.io_Data = data;
    r->io.iotd_Req.io_Error = 0;
    r->io.iotd_Req.io_Actual = 0;
    r->timed_out = 0;
}

static void rq_send(struct rq *r)
{
    r->state = 1;
    r->t0 = clk_now();
    SendIO((struct IORequest *)&r->io);
}

/* wait for every pending request; returns the number that hung */
static int rq_wait(void)
{
    ULONG mask = (1UL << dport.mp_SigBit) | (timer_ok ? (1UL << tport.mp_SigBit) : 0);
    int i, pend = 0, aborted = 0, nh = 0;
    struct Message *m;

    for (i = 0; i < NQ; i++)
        if (rq[i].state == 1)
            pend++;
    timeout_start(io_timeout);
    while (pend) {
        while ((m = GetMsg(&dport)) != 0) {
            for (i = 0; i < NQ; i++)
                if ((struct Message *)&rq[i].io == m && rq[i].state == 1) {
                    rq[i].state = 2;
                    rq[i].t1 = clk_now();
                    pend--;
                }
        }
        if (!pend)
            break;
        if (timeout_fired()) {
            WaitIO((struct IORequest *)&treq);
            timer_pending = 0;
            if (!aborted) {
                for (i = 0; i < NQ; i++)
                    if (rq[i].state == 1) {
                        rq[i].timed_out = 1;
                        AbortIO((struct IORequest *)&rq[i].io);
                    }
                aborted = 1;
                n_timeouts++;
                timeout_start(5);
                continue;
            }
            for (i = 0; i < NQ; i++)
                if (rq[i].state == 1) {
                    rq[i].state = 3;
                    nh++;
                }
            hung = 1;
            break;
        }
        Wait(mask);
    }
    timeout_stop();
    for (i = 0; i < NQ; i++)
        if (rq[i].state == 2) {
            ULONG dt = rq[i].t1 - rq[i].t0;
            if (n_lat < MAXLAT)
                lat_us[n_lat++] = to_us(dt);
            lat_sum += dt;
        }
    return nh;
}

/* one synchronous request with timeout; returns io_Error, or -100 when it
 * timed out (and was aborted), -101 when it hung */
static long rq_do(struct rq *r, UWORD cmd, ULONG off, ULONG len, APTR data)
{
    rq_prep(r, cmd, off, len, data);
    rq_send(r);
    if (rq_wait())
        return -101;
    r->state = 0;
    if (r->timed_out)
        return -100;
    return r->io.iotd_Req.io_Error;
}

/* exec's OpenDevice() for one particular device node (several nodes may
 * carry the same name, e.g. A4000 IDE and A4091 both "scsi.device") */
static void dev_open_node(struct Device *dev, ULONG unit, struct IORequest *io)
{
    register struct Device *a6 __asm("a6") = dev;
    register struct IORequest *a1 __asm("a1") = io;
    register ULONG d0 __asm("d0") = unit;
    register ULONG d1 __asm("d1") = 0;
    io->io_Device = dev;
    io->io_Error = 0;
    Forbid();
    __asm__ volatile ("jsr -6(a6)"
                      : "+r"(d0), "+r"(d1), "+r"(a1)
                      : "r"(a6)
                      : "a0", "cc", "memory");
    Permit();
    if (io->io_Error)
        io->io_Device = 0;
}

/* ------------------------------------------------------------ devices */
#define MAXDEV 8
struct devent {
    struct Device *dev;
    const char *name;
    char key[16];
    int nth;                    /* 0 = first node with that name */
    int is_td;
};
static struct devent devs[MAXDEV];
static int n_devs;

static const char *const dev_names[] = {
    "trackdisk.device", "scsi.device", "2nd.scsi.device", "3rd.scsi.device", "uaehf.device"
};
static const char *const dev_keys[] = { "td", "scsi", "scsi2", "scsi3", "uaehf" };

static void find_devices(void)
{
    struct Node *n;
    unsigned k;
    Forbid();
    for (n = SysBase->DeviceList.lh_Head; n->ln_Succ && n_devs < MAXDEV; n = n->ln_Succ) {
        if (!n->ln_Name)
            continue;
        for (k = 0; k < sizeof dev_names / sizeof dev_names[0]; k++)
            if (seq(n->ln_Name, dev_names[k])) {
                struct devent *d = &devs[n_devs];
                int i, nth = 0;
                char *p;
                for (i = 0; i < n_devs; i++)
                    if (seq(devs[i].name, dev_names[k]))
                        nth++;
                d->dev = (struct Device *)n;
                d->name = dev_names[k];
                d->nth = nth;
                d->is_td = (k == 0);
                p = scpy(d->key, dev_keys[k]);
                if (nth) {
                    *p++ = '_';
                    *p++ = (char)('1' + nth);
                    *p = 0;
                }
                n_devs++;
            }
    }
    Permit();
}

/* ------------------------------------------------------------ signature */
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

/* ------------------------------------------------------------ patterns */
static ULONG seed_g;
static UBYTE expbuf[BS] __attribute__((aligned(4)));

static void pat_block(UBYTE *p, ULONG lba, ULONG tid)
{
    UWORD *w = (UWORD *)p;      /* p is at least word aligned */
    ULONG x = (lba * 0x9e3779b1UL) ^ (tid * 0x85ebca6bUL) ^ seed_g;
    int i;
    if (!x)
        x = 1;
    w[0] = 0x414d;              /* "AMTD" */
    w[1] = 0x5444;
    w[2] = (UWORD)(lba >> 16);
    w[3] = (UWORD)lba;
    w[4] = (UWORD)(tid >> 16);
    w[5] = (UWORD)tid;
    w[6] = (UWORD)(seed_g >> 16);
    w[7] = (UWORD)seed_g;
    for (i = 8; i < BS / 2; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w[i] = (UWORD)(x >> 7);
    }
}

/* ------------------------------------------------------------ per unit */
struct unit {
    struct devent *d;
    ULONG unit;
    ULONG total;                /* blocks, from the signature */
    ULONG geom_total;           /* from the device, 0 if unknown */
    ULONG td_total, rc_total, nt_total;
    const char *geom_how;
    ULONG first, cursor, end;   /* writable region [first, end) */
    ULONG memtype;
    UBYTE *mem;                 /* AllocMem result */
    ULONG memsize;
    UBYTE *buf;                 /* 16-aligned, GUARD bytes after mem start */
    ULONG maxblk;
    ULONG tid;
    struct sig s0;
};

/* explicit bounds, no unsigned wrap (review M3): [first, end) is checked
 * non-empty before any write, and every block handed out lies in it */
static ULONG region_alloc(struct unit *u, ULONG n)
{
    ULONG lba;
    if (u->end <= u->first || n == 0 || n > u->end - u->first)
        return 0;
    if (u->cursor < u->first || u->cursor > u->end || n > u->end - u->cursor)
        u->cursor = u->first;
    lba = u->cursor;
    u->cursor += n;
    return lba;
}

/* ------------------------------------------------------------ ownership
 * Beyond the CONTRACT section 11 signature match: the fields that bound the
 * writable region (review M3), and the RDB area of a hardfile (review M2:
 * a partitioner keeps block 0 and the last block, so a test-owned RDB disk
 * that the user has partitioned still carries both signatures; its new
 * partitions lie exactly in [first_free, total-1)). Both return 0 if the
 * unit may be written, else the reason. */
#define KIND_ADF 1
#define KIND_HDF 2
#define KIND_RDB 3
#define KIND_SRAM 4
#define RDB_NONE 0xffffffffUL

static const char *owner_fields(const struct unit *u, const struct sig *sl)
{
    ULONG first = u->s0.first < 2 ? 2 : u->s0.first;
    if (sl->bsize != BS)
        return "the last block's signature has a block size other than 512; unit NOT written";
    if (sl->first != u->s0.first || sl->kind != u->s0.kind)
        return "first_free or kind differ between the first and the last signature block; unit NOT written";
    if (u->s0.kind < KIND_ADF || u->s0.kind > KIND_SRAM)
        return "unknown medium kind in the signature; unit NOT written";
    if ((u->s0.kind == KIND_ADF) != (u->d->is_td != 0))
        return "medium kind does not match the device (kind 1 = ADF is for trackdisk only); unit NOT written";
    if (first >= u->total - 1)
        return "first_free is at or beyond the last signed block: the writable region [max(first_free,2), "
               "total-1) is empty; unit NOT written";
    return 0;
}

static ULONG be32(const UBYTE *b, int o)
{
    return ((ULONG)b[o] << 24) | ((ULONG)b[o + 1] << 16) | ((ULONG)b[o + 2] << 8) | b[o + 3];
}

/* hardblocks.h: the block's SummedLongs longwords add up to 0 */
static int rdb_sum_ok(const UBYTE *b)
{
    ULONG n = be32(b, 4), sum = 0, i;
    if (n < 2 || n > BS / 4)
        return 0;
    for (i = 0; i < n; i++)
        sum += be32(b, (int)(i * 4));
    return sum == 0;
}

static int is_id(const UBYTE *b, const char *id)
{
    return b[0] == (UBYTE)id[0] && b[1] == (UBYTE)id[1] && b[2] == (UBYTE)id[2] && b[3] == (UBYTE)id[3];
}

static const char *owner_layout(struct unit *u)
{
    ULONG lba;
    long e;
    if (u->s0.kind == KIND_ADF || u->s0.kind == KIND_SRAM)
        return 0;               /* formatting rewrites block 0: no partitioning keeps the signature */
    /* the OS looks for an RDSK in blocks 0-15 (hardblocks.h RDB_LOCATION_LIMIT) */
    for (lba = 1; lba < 16 && lba < u->total - 1; lba++) {
        e = rq_do(&rq[0], CMD_READ, lba * BS, BS, u->buf);
        if (e)
            return "an RDB-area block (1-15) cannot be read; unit NOT written";
        if (!is_id(u->buf, "RDSK"))
            continue;
        if (u->s0.kind == KIND_HDF)
            return "a plain test-owned hardfile now carries a rigid disk block: the medium was partitioned; unit NOT written";
        if (lba != 1 || !rdb_sum_ok(u->buf))
            return "the RDSK block is not the test-owned one (block 1, valid checksum): the medium was repartitioned; unit NOT written";
        if (be32(u->buf, 32) != RDB_NONE || be32(u->buf, 24) != RDB_NONE)
            return "the RDSK block now lists filesystem headers or bad blocks: the medium was repartitioned; unit NOT written";
        {
            ULONG pl = be32(u->buf, 28);
            if (pl == RDB_NONE)
                return 0;
            if (pl != 2)
                return "the RDSK partition list does not start at block 2: the medium was repartitioned; unit NOT written";
        }
        /* the one PART media.py may write: AMTTEST, PBFF_NOMOUNT, DosType 0,
         * nothing after it */
        e = rq_do(&rq[0], CMD_READ, 2 * BS, BS, u->buf);
        if (e)
            return "the PART block cannot be read; unit NOT written";
        if (!is_id(u->buf, "PART") || !rdb_sum_ok(u->buf) || be32(u->buf, 16) != RDB_NONE ||
            !(be32(u->buf, 20) & 2) || be32(u->buf, 128 + 16 * 4) != 0 || u->buf[36] != 7 ||
            u->buf[37] != 'A' || u->buf[38] != 'M' || u->buf[39] != 'T' || u->buf[40] != 'T' ||
            u->buf[41] != 'E' || u->buf[42] != 'S' || u->buf[43] != 'T')
            return "the RDB has a partition other than the test-owned AMTTEST (NOMOUNT, DosType 0): the medium "
                   "was repartitioned; unit NOT written";
        return 0;
    }
    if (u->s0.kind == KIND_RDB)
        return "an RDB-kind medium without its RDSK block: the medium was rewritten; unit NOT written";
    return 0;
}

static const char *err_name(long e)
{
    switch (e) {
    case 0: return "ok";
    case -100: return "timeout (aborted)";
    case -101: return "hung (no reply after AbortIO)";
    case IOERR_OPENFAIL: return "IOERR_OPENFAIL";
    case IOERR_ABORTED: return "IOERR_ABORTED";
    case IOERR_NOCMD: return "IOERR_NOCMD";
    case IOERR_BADLENGTH: return "IOERR_BADLENGTH";
    case IOERR_BADADDRESS: return "IOERR_BADADDRESS";
    case IOERR_UNITBUSY: return "IOERR_UNITBUSY";
    case IOERR_SELFTEST: return "IOERR_SELFTEST";
    case TDERR_WriteProt: return "TDERR_WriteProt";
    case TDERR_DiskChanged: return "TDERR_DiskChanged";
    case TDERR_SeekError: return "TDERR_SeekError";
    case TDERR_NotSpecified: return "TDERR_NotSpecified";
    case TDERR_NoSecHdr: return "TDERR_NoSecHdr";
    case TDERR_BadSecPreamble: return "TDERR_BadSecPreamble";
    case TDERR_TooFewSecs: return "TDERR_TooFewSecs";
    case TDERR_NoMem: return "TDERR_NoMem";
    case TDERR_BadUnitNum: return "TDERR_BadUnitNum";
    case TDERR_BadDriveType: return "TDERR_BadDriveType";
    case TDERR_DriveInUse: return "TDERR_DriveInUse";
    case HFERR_SelfUnit: return "HFERR_SelfUnit";
    case HFERR_DMA: return "HFERR_DMA";
    case HFERR_Phase: return "HFERR_Phase";
    case HFERR_Parity: return "HFERR_Parity";
    case HFERR_SelTimeout: return "HFERR_SelTimeout";
    case HFERR_BadStatus: return "HFERR_BadStatus";
    case HFERR_NoBoard: return "HFERR_NoBoard";
    default: return "error";
    }
}

static void json_ioerr(const char *key, long e)
{
    json_obj(key);
    json_int("io_error", e);
    json_str("name", err_name(e));
    json_obj_end();
}

/* geometry: SCSI READ CAPACITY(10) (the emulated drive itself) for
 * scsi-style devices, TD_GETGEOMETRY (the driver's answer), and for
 * trackdisk TD_GETNUMTRACKS x sectors per track. The primary value is
 * READ CAPACITY, else TD_GETGEOMETRY, else TD_GETNUMTRACKS. */
static void unit_geometry(struct unit *u)
{
    static struct DriveGeometry g;
    static struct SCSICmd sc;
    static UBYTE cdb[10];
    long e;

    u->geom_total = u->td_total = u->rc_total = u->nt_total = 0;
    u->geom_how = "none";
    u->memtype = MEMF_PUBLIC;
    memset(&g, 0, sizeof g);
    e = rq_do(&rq[0], TD_GETGEOMETRY, 0, sizeof g, &g);
    if (e < -99)
        return;
    if (e == 0 && g.dg_SectorSize == BS && g.dg_TotalSectors) {
        u->td_total = g.dg_TotalSectors;
        if (g.dg_BufMemType)
            u->memtype = g.dg_BufMemType | MEMF_PUBLIC;
    }
    if (u->d->is_td) {
        ULONG tracks, type, spt;
        if (rq_do(&rq[0], TD_GETNUMTRACKS, 0, 0, 0) == 0) {
            tracks = rq[0].io.iotd_Req.io_Actual;
            if (rq_do(&rq[0], TD_GETDRIVETYPE, 0, 0, 0) == 0) {
                type = rq[0].io.iotd_Req.io_Actual;
                spt = (type == DRIVE3_5_150RPM) ? 22 : 11;
                u->nt_total = tracks * spt;
            }
        }
    } else {
        /* READ CAPACITY (10): last LBA and block length (SCSI-2 9.2.7) */
        UBYTE *d = expbuf;
        memset(cdb, 0, sizeof cdb);
        cdb[0] = 0x25;
        memset(&sc, 0, sizeof sc);
        memset(d, 0, 8);
        sc.scsi_Data = (UWORD *)d;
        sc.scsi_Length = 8;
        sc.scsi_Command = cdb;
        sc.scsi_CmdLength = 10;
        sc.scsi_Flags = SCSIF_READ;
        e = rq_do(&rq[0], HD_SCSICMD, 0, sizeof sc, &sc);
        if (e < -99)
            return;
        if (e == 0 && sc.scsi_Status == 0 && sc.scsi_Actual >= 8) {
            ULONG last = ((ULONG)d[0] << 24) | ((ULONG)d[1] << 16) | ((ULONG)d[2] << 8) | d[3];
            ULONG bl = ((ULONG)d[4] << 24) | ((ULONG)d[5] << 16) | ((ULONG)d[6] << 8) | d[7];
            if (bl == BS)
                u->rc_total = last + 1;
        }
    }
    if (u->rc_total) {
        u->geom_total = u->rc_total;
        u->geom_how = "READ_CAPACITY";
    } else if (u->td_total) {
        u->geom_total = u->td_total;
        u->geom_how = "TD_GETGEOMETRY";
    } else if (u->nt_total) {
        u->geom_total = u->nt_total;
        u->geom_how = "TD_GETNUMTRACKS";
    }
}

static void json_geom(struct unit *u)
{
    json_obj("ctx");
    json_str("how", u->geom_how);
    json_int("read_capacity", (long)u->rc_total);
    json_int("td_getgeometry", (long)u->td_total);
    json_int("td_getnumtracks", (long)u->nt_total);
    json_obj_end();
}

static int unit_alloc(struct unit *u, ULONG blocks)
{
    ULONG size = blocks * BS + 2 * GUARD + 32;
    if (SysBase->LibNode.lib_Version < 36)
        u->memtype = MEMF_CHIP | MEMF_PUBLIC;    /* V34 trackdisk / DMA */
    u->mem = AllocMem(size, u->memtype);
    if (!u->mem)
        return 0;
    u->memsize = size;
    u->buf = (UBYTE *)((((ULONG)u->mem + GUARD) + 15) & ~15UL);
    u->maxblk = blocks;
    return 1;
}

static void unit_free(struct unit *u)
{
    if (u->mem && !hung)
        FreeMem(u->mem, u->memsize);
    u->mem = 0;
    u->buf = 0;
}

static void td_motor_off(struct unit *u)
{
    if (u->d->is_td && !hung)
        rq_do(&rq[0], TD_MOTOR, 0, 0, 0);
}

/* trackdisk keeps a track buffer: CMD_UPDATE writes it out, CMD_CLEAR drops
 * it so the following read comes from the disk */
static long td_flush(struct unit *u)
{
    long e;
    if (!u->d->is_td)
        return 0;
    e = rq_do(&rq[0], CMD_UPDATE, 0, 0, 0);
    if (e)
        return e;
    return rq_do(&rq[0], CMD_CLEAR, 0, 0, 0);
}

/* verify n blocks at p (from lba, test tid); returns -1 or the bad block */
static long verify_blocks(const UBYTE *p, ULONG n, ULONG lba, ULONG tid,
                          ULONG *off, ULONG *ecrc, ULONG *gcrc)
{
    ULONG i, k, c1, c2;
    for (i = 0; i < n; i++) {
        pat_block(expbuf, lba + i, tid);
        c1 = crc32_update(0, expbuf, BS);
        c2 = crc32_update(0, p + i * BS, BS);
        if (c1 != c2) {
            for (k = 0; k < BS && expbuf[k] == p[i * BS + k]; k++)
                ;
            *off = k;
            *ecrc = c1;
            *gcrc = c2;
            return (long)i;
        }
    }
    return -1;
}

static long guard_check(const UBYTE *data, ULONG len)
{
    ULONG i;
    for (i = 1; i <= GUARD; i++)
        if (data[-(long)i] != 0xa5)
            return -(long)i;
    for (i = 0; i < GUARD; i++)
        if (data[len + i] != 0xa5)
            return (long)(len + i);
    return 0x7fffffffL;
}

static void perf_io(struct unit *u, const char *what, ULONG bytes, ULONG dt)
{
    json_begin("perf");
    json_str("mod", "devio");
    json_str("name", cid(u->d->key, u->unit, what));
    json_int("ops", (long)bytes);
    json_int("ticks", (long)to_eticks(dt));
    json_int("tick_hz", (long)(amt_eclock_hz() / 5));
    json_int("us", (long)to_us(dt));
    json_end();
}

static const char *const al_names[] = { "al", "w2", "odd" };
static const ULONG al_off[] = { 0, 2, 1 };

static void case_rw(struct unit *u, ULONG n, int al)
{
    char suffix[16], *p;
    const char *id;
    UBYTE *data;
    ULONG lba, tid, len = n * BS, wdt = 0, rdt = 0, fdt = 0, off = 0, ec = 0, gc = 0;
    long e, bad, g;
    const char *stage = 0;

    p = scpy(suffix, "rw");
    p = amt_utoa(p, n);
    *p++ = '_';
    scpy(p, al_names[al]);
    id = cid(u->d->key, u->unit, suffix);

    if (al == 2) {
        amt_case(id, AMT_ST_SKIP, "odd io_Data not allowed: the device documentation requires word-aligned buffers (devices/scsidisk.h scsi_Data, trackdisk.doc CMD_READ/CMD_WRITE); byte alignment is an optional driver feature");
        return;
    }
    if (hung) {
        amt_case(id, AMT_ST_SKIP, "an earlier request hung; no more I/O");
        return;
    }
    if (n > u->maxblk) {
        amt_case(id, AMT_ST_SKIP, "not enough memory for the buffer");
        return;
    }
    lba = region_alloc(u, n);
    if (!lba) {
        amt_case(id, AMT_ST_SKIP, "medium too small for this transfer");
        return;
    }
    tid = ++u->tid;
    data = u->buf + al_off[al];
    for (e = 0; e < (long)n; e++)
        pat_block(data + e * BS, lba + e, tid);

    e = rq_do(&rq[0], CMD_WRITE, lba * BS, len, data);
    wdt = rq[0].t1 - rq[0].t0;
    if (e || rq[0].io.iotd_Req.io_Actual != len) {
        stage = "write";
        goto fail;
    }
    if (u->d->is_td) {
        ULONG t0 = clk_now();
        e = td_flush(u);
        fdt = clk_now() - t0;
        if (e) {
            stage = "update";
            goto fail;
        }
    }
    memset(data - GUARD, 0xa5, GUARD);
    memset(data, 0x5a, len);
    memset(data + len, 0xa5, GUARD);
    e = rq_do(&rq[0], CMD_READ, lba * BS, len, data);
    rdt = rq[0].t1 - rq[0].t0;
    if (e || rq[0].io.iotd_Req.io_Actual != len) {
        stage = "read";
        goto fail;
    }
    bad = verify_blocks(data, n, lba, tid, &off, &ec, &gc);
    g = guard_check(data, len);
    if (bad >= 0 || g != 0x7fffffffL) {
        amt_case_open(id, AMT_ST_FAIL);
        json_obj("expect");
        json_str("data", "every block read back equal to what was written; guard bytes untouched");
        if (bad >= 0)
            json_hex("crc", ec);
        json_obj_end();
        json_obj("got");
        if (bad >= 0) {
            json_int("bad_block", bad);
            json_int("lba", (long)(lba + bad));
            json_int("first_bad_byte", (long)off);
            json_hex("crc", gc);
            json_hexn("exp_bytes", ((ULONG)expbuf[off & ~3UL] << 24) | ((ULONG)expbuf[(off & ~3UL) + 1] << 16) |
                                   ((ULONG)expbuf[(off & ~3UL) + 2] << 8) | expbuf[(off & ~3UL) + 3], 8);
            {
                const UBYTE *q = data + bad * BS + (off & ~3UL);
                json_hexn("got_bytes", ((ULONG)q[0] << 24) | ((ULONG)q[1] << 16) | ((ULONG)q[2] << 8) | q[3], 8);
            }
        }
        if (g != 0x7fffffffL)
            json_int("guard_hit_at", g);
        json_obj_end();
        json_str("why", bad >= 0 ? "data read back differs from the data written (CMD_WRITE then CMD_READ of the same blocks)"
                                 : "the device wrote outside io_Data[0..io_Length) (guard bytes changed)");
        goto info;
    }
    amt_case_open(id, AMT_ST_PASS);
    goto info;

fail:
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_int("io_error", 0);
    json_int("io_actual", (long)len);
    json_obj_end();
    json_obj("got");
    json_str("stage", stage);
    json_int("io_error", e);
    json_str("name", err_name(e));
    json_int("io_actual", (long)rq[0].io.iotd_Req.io_Actual);
    json_obj_end();
    json_str("why", e == -100 || e == -101
             ? "request did not complete in time (suspect a lost completion interrupt)"
             : "request on a test-owned unit returned an error or a short io_Actual");
info:
    json_obj("in");
    json_str("dev", u->d->name);
    json_int("nth", u->d->nth);
    json_int("unit", (long)u->unit);
    json_int("lba", (long)lba);
    json_int("blocks", (long)n);
    json_int("buf_mod4", (long)(((ULONG)data) & 3));
    json_int("tid", (long)tid);
    json_obj_end();
    json_obj("timing");
    json_int("write_us", (long)to_us(wdt));
    if (u->d->is_td)
        json_int("update_us", (long)to_us(fdt));
    json_int("read_us", (long)to_us(rdt));
    json_obj_end();
    amt_case_close();
    if (n == MAXBLK && al == 0 && !stage) {
        perf_io(u, "write255", len, wdt);
        perf_io(u, "read255", len, rdt);
    }
}

/* four requests in flight */
static void case_async(struct unit *u)
{
    const char *id = cid(u->d->key, u->unit, "async4");
    ULONG lba[NQ], tid, i, j, len = ASYNC_BLK * BS, off = 0, ec = 0, gc = 0;
    ULONG wl[NQ], rl[NQ];
    UBYTE *data[NQ];
    long bad = -1, badq = -1, e = 0;
    const char *stage = 0;

    if (hung) {
        amt_case(id, AMT_ST_SKIP, "an earlier request hung; no more I/O");
        return;
    }
    if (u->maxblk < NQ * (ASYNC_BLK + 1)) {
        amt_case(id, AMT_ST_SKIP, "not enough memory for the buffers");
        return;
    }
    tid = ++u->tid;
    for (i = 0; i < NQ; i++) {
        lba[i] = region_alloc(u, ASYNC_BLK);
        if (!lba[i]) {
            amt_case(id, AMT_ST_SKIP, "medium too small");
            return;
        }
        /* each buffer 2 bytes into its own slot (word aligned, not long) */
        data[i] = u->buf + i * (ASYNC_BLK + 1) * BS + (i & 1) * 2;
        for (j = 0; j < ASYNC_BLK; j++)
            pat_block(data[i] + j * BS, lba[i] + j, tid);
        rq_prep(&rq[i], CMD_WRITE, lba[i] * BS, len, data[i]);
    }
    for (i = 0; i < NQ; i++)
        rq_send(&rq[i]);
    if (rq_wait()) {
        stage = "write";
        e = -101;
        goto fail;
    }
    for (i = 0; i < NQ; i++) {
        rq[i].state = 0;
        wl[i] = to_us(rq[i].t1 - rq[i].t0);
        if (rq[i].timed_out || rq[i].io.iotd_Req.io_Error || rq[i].io.iotd_Req.io_Actual != len) {
            stage = "write";
            badq = (long)i;
            e = rq[i].timed_out ? -100 : rq[i].io.iotd_Req.io_Error;
        }
    }
    if (stage)
        goto fail;
    if (u->d->is_td && (e = td_flush(u)) != 0) {
        stage = "update";
        goto fail;
    }
    for (i = 0; i < NQ; i++) {
        memset(data[i], 0x5a, len);
        rq_prep(&rq[i], CMD_READ, lba[i] * BS, len, data[i]);
    }
    for (i = 0; i < NQ; i++)
        rq_send(&rq[i]);
    if (rq_wait()) {
        stage = "read";
        e = -101;
        goto fail;
    }
    for (i = 0; i < NQ; i++) {
        rq[i].state = 0;
        rl[i] = to_us(rq[i].t1 - rq[i].t0);
        if (rq[i].timed_out || rq[i].io.iotd_Req.io_Error || rq[i].io.iotd_Req.io_Actual != len) {
            stage = "read";
            badq = (long)i;
            e = rq[i].timed_out ? -100 : rq[i].io.iotd_Req.io_Error;
        }
    }
    if (stage)
        goto fail;
    for (i = 0; i < NQ && bad < 0; i++) {
        bad = verify_blocks(data[i], ASYNC_BLK, lba[i], tid, &off, &ec, &gc);
        if (bad >= 0)
            badq = (long)i;
    }
    if (bad >= 0) {
        amt_case_open(id, AMT_ST_FAIL);
        json_obj("expect");
        json_hex("crc", ec);
        json_obj_end();
        json_obj("got");
        json_int("request", badq);
        json_int("bad_block", bad);
        json_int("lba", (long)(lba[badq] + bad));
        json_int("first_bad_byte", (long)off);
        json_hex("crc", gc);
        json_obj_end();
        json_str("why", "with four requests in flight, data read back differs from the data written");
    } else {
        amt_case_open(id, AMT_ST_PASS);
    }
    goto info;
fail:
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_int("io_error", 0);
    json_int("io_actual", (long)len);
    json_obj_end();
    json_obj("got");
    json_str("stage", stage);
    json_int("request", badq);
    json_int("io_error", e);
    json_str("name", err_name(e));
    json_obj_end();
    json_str("why", e == -100 || e == -101
             ? "one of four requests in flight did not complete in time (suspect a lost completion interrupt)"
             : "one of four requests in flight returned an error or a short io_Actual");
info:
    json_obj("in");
    json_str("dev", u->d->name);
    json_int("unit", (long)u->unit);
    json_int("requests", NQ);
    json_int("blocks_each", ASYNC_BLK);
    json_arr("lba");
    for (i = 0; i < NQ; i++)
        json_int(NULL, (long)lba[i]);
    json_arr_end();
    json_obj_end();
    if (stage == 0 || (stage[0] == 'r')) {
        json_obj("timing");
        json_arr("write_us");
        for (i = 0; i < NQ; i++)
            json_int(NULL, (long)wl[i]);
        json_arr_end();
        if (!stage) {
            json_arr("read_us");
            for (i = 0; i < NQ; i++)
                json_int(NULL, (long)rl[i]);
            json_arr_end();
        }
        json_obj_end();
    }
    amt_case_close();
}

static void sort_ul(ULONG *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        ULONG v = a[i];
        for (j = i; j > 0 && a[j - 1] > v; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
}

static void perf_latency(struct unit *u)
{
    if (!n_lat)
        return;
    sort_ul(lat_us, n_lat);
    json_begin("perf");
    json_str("mod", "devio");
    json_str("name", cid(u->d->key, u->unit, "latency"));
    json_int("ops", n_lat);
    json_int("ticks", (long)to_eticks(lat_sum));
    json_int("tick_hz", (long)(amt_eclock_hz() / 5));
    json_int("min_us", (long)lat_us[0]);
    json_int("p50_us", (long)lat_us[n_lat / 2]);
    json_int("p90_us", (long)lat_us[(n_lat * 9) / 10]);
    json_int("max_us", (long)lat_us[n_lat - 1]);
    json_int("timeouts", n_timeouts);
    json_end();
}

/* one opened unit */
static void test_unit(struct devent *d, ULONG unit)
{
    static struct unit U;
    struct unit *u = &U;
    struct sig sl;
    long e;
    int i, owned;
    ULONG nb;

    memset(u, 0, sizeof *u);
    u->d = d;
    u->unit = unit;
    n_lat = 0;
    n_timeouts = 0;
    lat_sum = 0;
    io_timeout = d->is_td ? 60 : 20;

    if (d->is_td) {
        e = rq_do(&rq[0], TD_CHANGESTATE, 0, 0, 0);
        if (e == 0 && rq[0].io.iotd_Req.io_Actual != 0) {
            amt_case(cid(d->key, unit, 0), AMT_ST_SKIP, "no disk in the drive");
            return;
        }
    }
    unit_geometry(u);
    if (hung)
        goto out;
    /* the full buffer, or less if memory is short */
    for (nb = MAXBLK; nb >= 16; nb = nb / 2)
        if (unit_alloc(u, nb))
            break;
    if (!u->mem) {
        amt_case(cid(d->key, unit, 0), AMT_ST_SKIP, "no memory for an I/O buffer");
        goto out;
    }

    /* block 0: ownership */
    e = rq_do(&rq[0], CMD_READ, 0, BS, u->buf);
    owned = (e == 0 && sig_parse(u->buf, &u->s0) && u->s0.lba == 0 && u->s0.bsize == BS &&
             u->s0.total > 4);
    if (!owned) {
        if (amt_par.flags & AMT_F_UNSAFE) {
            amt_case_open(cid(d->key, unit, "read0"), e == 0 ? AMT_ST_PASS : AMT_ST_FAIL);
            if (e) {
                json_obj("expect");
                json_int("io_error", 0);
                json_obj_end();
                json_ioerr("got", e);
                json_str("why", "reading block 0 of an unowned unit failed (unsafe mode, read only)");
            }
            json_obj("ctx");
            json_str("geometry", u->geom_how);
            json_int("blocks", (long)u->geom_total);
            if (e == 0)
                json_hex("crc0", crc32_update(0, u->buf, BS));
            json_obj_end();
            amt_case_close();
        } else {
            amt_case(cid(d->key, unit, 0), AMT_ST_SKIP, e == 0 ? "not test-owned"
                     : "not test-owned (block 0 could not be read)");
        }
        goto out;
    }

    /* io_Offset is 32 bits: blocks beyond 4 GB are out of reach of CMD_READ/
     * CMD_WRITE (TD64/NSD not used), so a larger medium is refused (review M3) */
    u->total = u->s0.total;
    if (u->total > 0x00800000UL) {
        amt_case_open(cid(d->key, unit, "owner"), AMT_ST_SKIP);
        json_str("why", "signed medium larger than 4 GB: its last block is beyond the 32-bit io_Offset "
                        "(TD64/NSD are not used); unit NOT written");
        json_obj("ctx");
        json_int("blocks", (long)u->total);
        json_obj_end();
        amt_case_close();
        goto out;
    }
    /* the last block must be signed too */
    e = rq_do(&rq[0], CMD_READ, (u->total - 1) * BS, BS, u->buf);
    if (e || !sig_parse(u->buf, &sl) || sl.lba != u->total - 1 || sl.total != u->total) {
        amt_case_open(cid(d->key, unit, "owner"), AMT_ST_FAIL);
        json_obj("expect");
        json_str("last_block", "AMTOWN01 signature, same UUID");
        json_int("lba", (long)(u->total - 1));
        json_obj_end();
        json_obj("got");
        json_int("io_error", e);
        json_str("name", err_name(e));
        json_int("signed", e == 0 && sig_parse(u->buf, &sl));
        json_obj_end();
        json_str("why", "block 0 says this is a test-owned medium of N blocks, but block N-1 is not readable or not signed (wrong capacity or data mapping); unit NOT written");
        json_obj("ctx");
        json_str("geometry", u->geom_how);
        json_int("geometry_blocks", (long)u->geom_total);
        json_obj_end();
        amt_case_close();
        goto out;
    }
    for (i = 0; i < 16; i++)
        if (sl.uuid[i] != u->s0.uuid[i])
            break;
    if (i < 16) {
        amt_case(cid(d->key, unit, "owner"), AMT_ST_FAIL,
                 "first and last block carry different run UUIDs; unit NOT written");
        goto out;
    }
    /* the fields that bound the writes must agree and make sense (review M3) */
    {
        const char *bad = owner_fields(u, &sl);
        if (!bad)
            bad = owner_layout(u);
        if (hung)
            goto out;
        if (bad) {
            amt_case_open(cid(d->key, unit, "owner"), AMT_ST_FAIL);
            json_str("why", bad);
            json_obj("got");
            json_int("blocks", (long)u->total);
            json_int("first_free", (long)u->s0.first);
            json_int("first_free_last", (long)sl.first);
            json_int("kind", (long)u->s0.kind);
            json_int("kind_last", (long)sl.kind);
            json_int("block_size_last", (long)sl.bsize);
            json_obj_end();
            amt_case_close();
            goto out;
        }
    }
    amt_case_open(cid(d->key, unit, "owner"), AMT_ST_PASS);
    json_obj("in");
    json_str("dev", d->name);
    json_int("unit", (long)unit);
    json_int("blocks", (long)u->total);
    json_int("first_free", (long)u->s0.first);
    json_int("kind", (long)u->s0.kind);
    json_int("buf_blocks", (long)u->maxblk);
    json_hex("memtype", u->memtype);
    json_obj_end();
    amt_case_close();

    /* the device's idea of the capacity */
    if (u->geom_total == u->total) {
        amt_case_open(cid(d->key, unit, "geometry"), AMT_ST_PASS);
        json_geom(u);
        amt_case_close();
    } else if (!u->geom_total)
        amt_case(cid(d->key, unit, "geometry"), AMT_ST_UNTESTABLE,
                 "the device answers neither READ CAPACITY nor TD_GETGEOMETRY / TD_GETNUMTRACKS");
    else {
        amt_case_open(cid(d->key, unit, "geometry"), AMT_ST_FAIL);
        json_obj("expect");
        json_int("blocks", (long)u->total);
        json_obj_end();
        json_obj("got");
        json_int("blocks", (long)u->geom_total);
        json_str("how", u->geom_how);
        json_obj_end();
        json_str("why", "capacity reported by the drive differs from the size of the medium (signature in block 0 and in the last block, which reads fine)");
        json_geom(u);
        amt_case_close();
    }
    /* the driver's TD_GETGEOMETRY, when READ CAPACITY was the primary */
    if (u->rc_total && u->td_total) {
        if (u->td_total == u->total) {
            amt_case(cid(d->key, unit, "geometry_td"), AMT_ST_PASS, 0);
        } else {
            amt_case_open(cid(d->key, unit, "geometry_td"), AMT_ST_FAIL);
            json_obj("expect");
            json_int("dg_TotalSectors", (long)u->total);
            json_obj_end();
            json_obj("got");
            json_int("dg_TotalSectors", (long)u->td_total);
            json_obj_end();
            json_str("why", "TD_GETGEOMETRY dg_TotalSectors must be the number of sectors on the drive (trackdisk.h DriveGeometry); READ CAPACITY and the signed last block agree on the size");
            json_geom(u);
            amt_case_close();
        }
    }

    if (d->is_td) {
        e = rq_do(&rq[0], TD_PROTSTATUS, 0, 0, 0);
        if (e == 0 && rq[0].io.iotd_Req.io_Actual != 0) {
            amt_case(cid(d->key, unit, "write"), AMT_ST_SKIP, "disk is write-protected");
            goto out;
        }
    }

    u->first = u->s0.first < 2 ? 2 : u->s0.first;
    u->end = u->total - 1;
    u->cursor = u->first;
    {
        static const UWORD sizes[] = { 1, 2, 7, 16, 64, 255 };
        unsigned si;
        int al;
        for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++)
            for (al = 0; al < 3; al++)
                case_rw(u, sizes[si], al);
    }
    case_async(u);
    perf_latency(u);
out:
    td_motor_off(u);
    unit_free(u);
}

void amt_run_devio(void)
{
    int i, k;
    ULONG unit;
    static char found[512];
    char *fp;

    seed_g = amt_par.seed;
    if (!port_init(&dport)) {
        amt_case("setup", AMT_ST_UNTESTABLE, "no free signal for a message port");
        return;
    }
    timer_open();
    find_devices();

    /* what exists (deterministic per configuration) */
    amt_case_open("devices", AMT_ST_PASS);
    json_obj("in");
    json_arr("nodes");
    for (i = 0; i < n_devs; i++) {
        fp = scpy(found, devs[i].key);
        fp = scpy(fp, " ");
        fp = amt_vstr(fp, devs[i].dev->dd_Library.lib_Version, devs[i].dev->dd_Library.lib_Revision);
        json_str(NULL, found);
    }
    json_arr_end();
    json_arr("all");
    {
        struct Node *n;
        Forbid();
        for (n = SysBase->DeviceList.lh_Head; n->ln_Succ; n = n->ln_Succ)
            json_str(NULL, n->ln_Name ? n->ln_Name : "?");
        Permit();
    }
    json_arr_end();
    json_int("timer_eclock", have_eclock);
    json_obj_end();
    amt_case_close();

    for (i = 0; i < n_devs && !hung; i++) {
        struct devent *d = &devs[i];
        ULONG nunits = d->is_td ? 4 : 8;
        for (unit = 0; unit < nunits && !hung; unit++) {
            for (k = 0; k < NQ; k++) {
                memset(&rq[k], 0, sizeof rq[k]);
                rq[k].io.iotd_Req.io_Message.mn_ReplyPort = &dport;
                rq[k].io.iotd_Req.io_Message.mn_Length = sizeof(struct IOExtTD);
            }
            if (d->nth == 0) {
                if (OpenDevice((CONST_STRPTR)d->name, unit, (struct IORequest *)&rq[0].io, 0) != 0)
                    continue;
            } else {
                dev_open_node(d->dev, unit, (struct IORequest *)&rq[0].io);
                if (!rq[0].io.iotd_Req.io_Device)
                    continue;
            }
            for (k = 1; k < NQ; k++) {
                rq[k].io.iotd_Req.io_Device = rq[0].io.iotd_Req.io_Device;
                rq[k].io.iotd_Req.io_Unit = rq[0].io.iotd_Req.io_Unit;
            }
            test_unit(d, unit);
            if (!hung)
                CloseDevice((struct IORequest *)&rq[0].io);
        }
    }
    if (hung)
        amt_log("devio: a request hung; remaining units not tested, the IORequest is left open");
    timer_close();
    if (!hung)
        FreeSignal(dport.mp_SigBit);
    if (tport.mp_SigBit)
        FreeSignal(tport.mp_SigBit);
    if (cport.mp_SigBit)
        FreeSignal(cport.mp_SigBit);
}
