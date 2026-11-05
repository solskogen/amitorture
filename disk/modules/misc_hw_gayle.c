/* misc_hw part: Gayle (A600, A1200): ID, interrupt/status/enable registers,
 * IDE task file (no command that writes media), PCMCIA attribute memory
 * (CIS) and, on a signed test-owned SRAM card only, common memory.
 *
 * Register map (Gayle notes in the Linux/NetBSD drivers, A600/A1200
 * technical notes):
 *   $DE1000  ID: a write resets the sequence, then each read returns the next
 *            ID bit in bit 7, MSB first; the first four reads give 1101, the
 *            eight give $D0 (A600) or $D1 (A1200)
 *   $DA8000  card status: b7 IDE INTRQ (live), b6 CCDET card detect, b5 BVD1/
 *            SC, b4 BVD2/DA, b3 WR write enable, b2 BSY/IRQ; b1 DAEN, b0 DIS
 *   $DA9000  interrupt request latch, same bit layout; writing 0 clears a
 *            bit, writing 1 leaves it; b1 RESET and b0 BERR (on card change)
 *   $DAA000  interrupt enable, same layout; b1 BVD level, b0 BSY level
 *            (0 = INT2, 1 = INT6)
 *   $DAB000  config: b1-0 card voltage, b3-2 access speed (never written)
 *   $DA0000  IDE data (16 bit); task file registers at $DA0002 + 4n
 *            (n = 1 error, 2 sector count, 3 sector number/LBA 7-0, 4 cyl low/
 *            LBA 15-8, 5 cyl high/LBA 23-16, 6 device/head, 7 status/command),
 *            alternate status / device control at $DA101A; the data bytes are
 *            crossed (IDE D7-D0 on CPU D15-D8: sector byte 0 at the even
 *            address, so media keep the PC byte order and IDENTIFY words read
 *            byte-swapped)
 *   $A00000  PCMCIA attribute memory (CIS on even bytes), $600000 common memory
 *
 * Gayle is detected through the OS: card.resource exists only in the
 * Kickstarts of Gayle machines. Nothing at $DA0000-$DEFFFF is touched
 * otherwise (on a Gary machine $DE1000 is the Gary timeout register).
 * Never written: the card status register (DIS would cut the slot), the
 * config register (card voltage), IRQ bits 1-0 (they are written back as
 * read). Saved and restored: interrupt enable, the IDE task file registers
 * and device/head; the IRQ latch bits this part caused are cleared. */
#include "misc_hw.h"
#include "hardware_audit.h"

#define G_ID    0xde1000UL
#define G_CS    0xda8000UL
#define G_IRQ   0xda9000UL
#define G_INT   0xdaa000UL
#define G_CFG   0xdab000UL
#define IDE     0xda0000UL
#define IDE_R(n) (IDE + 2 + 4UL * (n))
#define IDE_ALT (IDE + 0x101a)
#define ATTR    0xa00000UL
#define COMMON  0x600000UL
#define COMMON_MAX 0x400000UL

#define R_ERR 1
#define R_NSECT 2
#define R_SECT 3
#define R_LCYL 4
#define R_HCYL 5
#define R_SEL 6
#define R_STAT 7

#define ST_BSY 0x80
#define ST_DRDY 0x40
#define ST_DF 0x20
#define ST_DRQ 0x08
#define ST_ERR 0x01

#define INTB_PORTS 0x0008
#define INTB_EXTER 0x2000

ULONG crc32_update(ULONG crc, const void *buf, ULONG len);

static UBYTE g_int0, g_irq0, g_cs0;

static void rep8(const char *id, int ok, const char *k, UBYTE e, UBYTE g, const char *why)
{
    if (ok) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_hexn(k, e, 2);
    json_obj_end();
    json_obj("got");
    json_hexn(k, g, 2);
    json_obj_end();
    json_str("why", why);
    amt_case_close();
}

/* ------------------------------------------------------------ ID */
/* A write to $DE1000 resets the sequence; then each read gives the next ID
 * bit in bit 7, MSB first. The first four reads are 1101 on every Gayle:
 * that nibble ($D) is what Kickstart 3.1 checks (the Gayle detection in
 * scsi.device/card.resource: four reads, cmpi.b #$0D). Exec's ReadGayle()
 * reads eight and returns the byte: the low nibble is the revision, 0 on
 * the A600's Gayle (ECS) and 1 on the A1200's (AGA, "AA Gayle"), so the
 * byte is $D0 on an A600 and $D1 on an A1200. */
static UBYTE gayle_id_read(int n)
{
    UBYTE id = 0;
    int i;
    hw_wr8(G_ID, 0);
    for (i = 0; i < n; i++)
        id = (UBYTE)((id << 1) | ((hw_rd8(G_ID) >> 7) & 1));
    return id;
}

static void t_id(void)
{
    int aga = amt_probe_result.chipset && amt_probe_result.chipset[0] == 'A';
    UBYTE exp = aga ? 0xd1 : 0xd0;
    UBYTE a = gayle_id_read(8), n4 = gayle_id_read(4), b = gayle_id_read(8);
    if (a == exp && n4 == 0xd && b == exp) {
        amt_case_open("gayle_id", AMT_ST_PASS);
        json_obj("ctx");
        json_hexn("id8", a, 2);
        json_obj_end();
        amt_case_close();
        return;
    }
    amt_case_open("gayle_id", AMT_ST_FAIL);
    json_obj("expect");
    json_hexn("id8_first", exp, 2);
    json_hexn("id4", 0xd, 1);
    json_hexn("id8_after_4", exp, 2);
    json_obj_end();
    json_obj("got");
    json_hexn("id8_first", a, 2);
    json_hexn("id4", n4, 1);
    json_hexn("id8_after_4", b, 2);
    json_obj_end();
    json_str("why", aga ? "Gayle ID: after a write to $DE1000, reads of bit 7 return the ID MSB first: "
                          "1101 on every Gayle (what Kickstart checks), then 0001: the A1200 (AGA) Gayle is $D1; "
                          "a write restarts the sequence"
                        : "Gayle ID: after a write to $DE1000, reads of bit 7 return the ID MSB first: "
                          "1101 on every Gayle (what Kickstart checks), then 0000: the A600 (ECS) Gayle is $D0; "
                          "a write restarts the sequence");
    json_obj("in");
    json_str("seq", "write $DE1000, 8 reads; write, 4 reads; write, 8 reads");
    json_str("chipset", aga ? "AGA (A1200 Gayle)" : "OCS/ECS (A600 Gayle)");
    json_obj_end();
    amt_case_close();
}

/* ------------------------------------------------------------ registers */
static void t_regs(void)
{
    static const UBYTE pat[] = { 0x00, 0xff, 0x55, 0xaa, 0x80, 0x01, 0x7e };
    UBYTE got = 0, r1;
    int i, bad = -1;

    /* interrupt enable: read/write latch */
    for (i = 0; i < (int)sizeof(pat); i++) {
        hw_wr8(G_INT, pat[i]);
        got = hw_rd8(G_INT);
        if (got != pat[i]) {
            bad = i;
            break;
        }
    }
    hw_wr8(G_INT, g_int0);
    rep8("gayle_int_enable_rw", bad < 0, "int", bad < 0 ? 0 : pat[bad], got,
         "$DAA000 interrupt enable is a read/write register (all 8 bits)");

    /* IRQ latch: writing 1 leaves a bit as it is */
    hw_wr8(G_IRQ, (UBYTE)(0xfc | (g_irq0 & 3)));
    r1 = hw_rd8(G_IRQ);
    rep8("gayle_irq_write1_keeps", r1 == g_irq0, "irq", g_irq0, r1,
         "$DA9000: writing 1 to a request bit must not set or clear it");
    /* writing 0 clears: bit 7 (IDE) and every bit that was not latched get
     * 0; a pending card-change/BVD/BSY latch (bits 6-2) is written 1 so it
     * stays for card.resource (review L18). The IDE latch may come back
     * while its INTRQ is live. */
    hw_wr8(G_IRQ, (UBYTE)(g_irq0 & 0x7f));
    r1 = hw_rd8(G_IRQ);
    {
        UBYTE allow = (hw_rd8(G_CS) & 0x80) ? 0x80 : 0;
        rep8("gayle_irq_write0_clears", (r1 & ~allow) == (g_irq0 & 0x7f), "irq",
             (UBYTE)(g_irq0 & 0x7f), r1,
             "$DA9000: writing 0 to a request bit clears it, writing 1 keeps it (IDE allowed while its INTRQ is live)");
    }
}

/* ------------------------------------------------------------ IDE */
static UBYTE ide_sv[8];
static UWORD idw[256];

/* ATA-4 7.15.6 / 9: the command block registers may be written only with
 * BSY = 0 and DRQ = 0; DRQ = 1 means a data transfer of someone's command
 * is waiting (the OS driver's, if the takeover caught it) */
static int ide_idle(void)
{
    return (hw_rd8(IDE_ALT) & (ST_BSY | ST_DRQ)) == 0;
}

static int ide_wait_notbusy(ULONG ms)
{
    ULONG t0 = amt_ticks(), lim = mh_cck_ms(ms);
    while (hw_rd8(IDE_ALT) & ST_BSY)
        if (amt_ticks() - t0 > lim)
            return 0;
    return 1;
}

static void ide_restore(void)
{
    hw_wr8(IDE_R(R_SEL), ide_sv[R_SEL]);
    hw_wr8(IDE_R(R_NSECT), ide_sv[R_NSECT]);
    hw_wr8(IDE_R(R_SECT), ide_sv[R_SECT]);
    hw_wr8(IDE_R(R_LCYL), ide_sv[R_LCYL]);
    hw_wr8(IDE_R(R_HCYL), ide_sv[R_HCYL]);
}

static const char *const regname[8] = { "data", "error", "nsect", "sector", "lcyl", "hcyl", "select", "status" };

static void t_ide_taskfile(void)
{
    int set, r, bad = -1, badr = 0;
    UBYTE v[4], got = 0, exp = 0;
    int nsets = AMT_QUICK() ? 6 : 12;
    for (set = 0; set < nsets && bad < 0; set++) {
        for (r = 0; r < 4; r++) {
            switch (set) {
            case 0: v[r] = 0x00; break;
            case 1: v[r] = 0xff; break;
            case 2: v[r] = (r & 1) ? 0xaa : 0x55; break;
            case 3: v[r] = (UBYTE)(1 << (r * 2)); break;
            case 4: v[r] = (UBYTE)~(1 << (r * 2 + 1)); break;
            default: v[r] = (UBYTE)amt_rand(); break;
            }
            hw_wr8(IDE_R(R_NSECT + r), v[r]);
        }
        for (r = 0; r < 4; r++) {
            got = hw_rd8(IDE_R(R_NSECT + r));
            if (got != v[r]) {
                bad = set;
                badr = r;
                exp = v[r];
                break;
            }
        }
    }
    if (bad < 0) {
        amt_case("ide_taskfile_rw", AMT_ST_PASS, 0);
    } else {
        amt_case_open("ide_taskfile_rw", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn(regname[R_NSECT + badr], exp, 2);
        json_obj_end();
        json_obj("got");
        json_hexn(regname[R_NSECT + badr], got, 2);
        json_obj_end();
        json_str("why", "sector count / LBA low/mid/high read back what was last written (ATA task file, no command issued)");
        json_obj("in");
        json_int("set", bad);
        json_hexn("nsect", v[0], 2); json_hexn("sector", v[1], 2);
        json_hexn("lcyl", v[2], 2); json_hexn("hcyl", v[3], 2);
        json_obj_end();
        amt_case_close();
    }
    /* device/head: head bits 3-0 read back, device bit 4 unchanged */
    {
        UBYTE s0 = ide_sv[R_SEL], g = 0, h;
        int okh = 1;
        for (h = 0; h < 16 && okh; h++) {
            hw_wr8(IDE_R(R_SEL), (UBYTE)((s0 & 0xf0) | h));
            g = hw_rd8(IDE_R(R_SEL));
            if ((g & 0x1f) != ((s0 & 0x10) | h))
                okh = 0;
        }
        hw_wr8(IDE_R(R_SEL), s0);
        rep8("ide_select_head_rw", okh, "select", okh ? 0 : (UBYTE)((s0 & 0xf0) | (h - 1)), g,
             "device/head register: bits 3-0 (head / LBA 27-24) read back, device bit 4 kept");
    }
}

static void t_ide_status(void)
{
    UBYTE alt = hw_rd8(IDE_ALT), st = hw_rd8(IDE_R(R_STAT));
    /* a PACKET (ATAPI) device keeps DRDY = 0 until IDENTIFY PACKET DEVICE
     * (ATA/ATAPI-4 9.1); it shows the 14h/EBh signature in LBA mid/high
     * after reset (review L17) */
    UBYTE drdy = (ide_sv[R_LCYL] == 0x14 && ide_sv[R_HCYL] == 0xeb) ? (UBYTE)(alt & ST_DRDY) : ST_DRDY;
    rep8("ide_status_idle", (alt & (ST_BSY | ST_DF | ST_DRQ | ST_DRDY)) == drdy, "status", drdy, alt,
         "idle ATA device: BSY=0, DRDY=1 (ATAPI with the 14/EB signature: DRDY 0 or 1), DF=0, DRQ=0 (ATA-4 status register)");
    rep8("ide_altstatus_eq_status", (alt & ~ST_ERR) == (st & ~ST_ERR), "status", alt, st,
         "the alternate status register shows the same bits as status (reading it does not clear INTRQ)");
}

static int lba48;

static void t_ide_identify(void)
{
    UBYTE st, cs1, irq1, cs2, irq2, irqc, lc, hc;
    UWORD req1;
    ULONG t0;
    int i, okd, sumok = 1, drq;
    UBYTE sum = 0;

    lba48 = -1;
    if (!ide_idle()) {
        amt_case("ide_identify", AMT_ST_SKIP, "BSY or DRQ set before the command: never issue a command then (ATA-4 7.15.6)");
        amt_case("ide_intrq_gayle", AMT_ST_SKIP, "IDENTIFY not issued");
        return;
    }
    hw_wr8(G_INT, (UBYTE)(g_int0 | 0x80));
    hw_wr8(G_IRQ, (UBYTE)(0x7c | (g_irq0 & 3)));
    CREG(R_INTREQ) = INTB_PORTS;
    hw_wr8(IDE_R(R_STAT), 0xec);            /* IDENTIFY DEVICE: reads only */
    t0 = amt_ticks();
    while (amt_ticks() - t0 < mh_cck_ms(1)) /* 400 ns before status is valid */
        ;
    okd = ide_wait_notbusy(2000);
    st = hw_rd8(IDE_ALT);
    mh_wait_cck(mh_cck_ms(1));
    cs1 = hw_rd8(G_CS);
    irq1 = hw_rd8(G_IRQ);
    req1 = CREG(R_INTREQR);
    drq = okd && (st & ST_DRQ) && !(st & ST_ERR);
    lc = hw_rd8(IDE_R(R_LCYL));
    hc = hw_rd8(IDE_R(R_HCYL));
    if (okd && (st & ST_ERR) && lc == 0x14 && hc == 0xeb) {
        hw_rd8(IDE_R(R_STAT));
        hw_wr8(G_IRQ, (UBYTE)(0x7c | (g_irq0 & 3)));
        hw_wr8(G_INT, g_int0);
        amt_case("ide_identify", AMT_ST_SKIP, "ATAPI device (signature 14/EB): IDENTIFY DEVICE is aborted by design");
        amt_case("ide_intrq_gayle", AMT_ST_SKIP, "ATAPI device");
        amt_case("ide_lba48_hob", AMT_ST_SKIP, "ATAPI device");
        return;
    }
    /* Gayle crosses the data bytes (sector byte 0 at the even address, the
     * PC byte order of the medium), so IDENTIFY words arrive byte-swapped */
    for (i = 0; i < 256; i++) {
        UWORD w = drq ? hw_rd16(IDE) : 0;
        idw[i] = (UWORD)((w << 8) | (w >> 8));
    }
    mh_wait_cck(mh_cck_ms(1));
    st = drq ? hw_rd8(IDE_R(R_STAT)) : st;   /* clears INTRQ */
    if (!drq)
        hw_rd8(IDE_R(R_STAT));
    mh_wait_cck(mh_cck_ms(1));
    cs2 = hw_rd8(G_CS);
    irq2 = hw_rd8(G_IRQ);
    hw_wr8(G_IRQ, (UBYTE)(0x7c | (g_irq0 & 3)));
    irqc = hw_rd8(G_IRQ);
    hw_wr8(G_INT, g_int0);

    if ((idw[255] & 0xff) == 0xa5) {
        for (i = 0; i < 256; i++)
            sum = (UBYTE)(sum + (idw[i] >> 8) + (idw[i] & 0xff));
        sumok = sum == 0;
    }
    if (drq && !(idw[0] & 0x8000) && sumok && !(st & (ST_BSY | ST_DRQ | ST_ERR))) {
        amt_case_open("ide_identify", AMT_ST_PASS);
        json_obj("ctx");
        json_hexn("w0", idw[0], 4);
        json_int("cyl", idw[1]); json_int("heads", idw[3]); json_int("spt", idw[6]);
        json_hexn("w49", idw[49], 4);
        json_int("lba_sectors", (long)(((ULONG)idw[61] << 16) | idw[60]));
        json_hexn("w83", idw[83], 4);
        json_int("checksum", (idw[255] & 0xff) == 0xa5);
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open("ide_identify", AMT_ST_FAIL);
        json_obj("expect");
        json_str("after_command", "BSY=0 DRQ=1 ERR=0, 256 words, word 0 bit 15 = 0, word 255 checksum if A5, then status BSY=DRQ=ERR=0");
        json_obj_end();
        json_obj("got");
        json_int("not_busy", okd);
        json_int("drq", drq);
        json_hexn("w0", idw[0], 4);
        json_hexn("w255", idw[255], 4);
        json_int("checksum_ok", sumok);
        json_hexn("status_after", st, 2);
        json_obj_end();
        json_str("why", "IDENTIFY DEVICE (ECh, ATA-4 8.12) did not complete as an ATA device must");
        amt_case_close();
    }
    if (drq)
        lba48 = (idw[83] & 0x4000) && !(idw[83] & 0x8000) && (idw[83] & 0x0400);

    /* interrupt path: INTRQ -> Gayle CS b7 (live) and IRQ b7 (latched) ->
     * INT2 (Paula PORTS) when enabled; status read drops INTRQ; IRQ b7 is
     * cleared by writing 0 */
    {
        int ok = (cs1 & 0x80) && (irq1 & 0x80) && (req1 & INTB_PORTS) && !(cs2 & 0x80) &&
                 (irq2 & 0x80) && !(irqc & 0x80);
        if (!drq) {
            amt_case("ide_intrq_gayle", AMT_ST_UNTESTABLE, "IDENTIFY did not complete (see ide_identify)");
        } else if (ok) {
            amt_case("ide_intrq_gayle", AMT_ST_PASS, 0);
        } else {
            amt_case_open("ide_intrq_gayle", AMT_ST_FAIL);
            json_obj("expect");
            json_str("cs_b7", "1 after the command, 0 after the status read");
            json_str("irq_b7", "1 (latched) until written 0, then 0");
            json_str("intreq_ports", "1 with $DAA000 b7 = 1");
            json_obj_end();
            json_obj("got");
            json_hexn("cs_cmd", cs1, 2);
            json_hexn("irq_cmd", irq1, 2);
            json_hexn("intreqr_cmd", req1, 4);
            json_hexn("cs_after_status", cs2, 2);
            json_hexn("irq_after_status", irq2, 2);
            json_hexn("irq_after_clear", irqc, 2);
            json_obj_end();
            json_str("why", "the IDE interrupt did not travel INTRQ -> Gayle status/latch -> INT2 as documented");
            amt_case_close();
        }
    }
}

/* ATA/ATAPI-6 6.x: with 48-bit support each of these registers is a
 * two-byte FIFO; device control HOB=1 reads the previously written byte */
static void t_ide_hob(void)
{
    UBYTE a, b, c, d;
    if (lba48 < 0) {
        amt_case("ide_lba48_hob", AMT_ST_UNTESTABLE, "IDENTIFY did not complete (see ide_identify)");
        return;
    }
    if (!lba48) {
        amt_case("ide_lba48_hob", AMT_ST_SKIP, "device does not report the 48-bit feature set (IDENTIFY word 83 bit 10)");
        return;
    }
    hw_wr8(IDE_R(R_NSECT), 0x12);
    hw_wr8(IDE_R(R_NSECT), 0x34);
    hw_wr8(IDE_R(R_SECT), 0x56);
    hw_wr8(IDE_R(R_SECT), 0x78);
    hw_wr8(IDE_ALT, 0x80);                 /* HOB, nIEN=0, SRST=0 */
    a = hw_rd8(IDE_R(R_NSECT));
    b = hw_rd8(IDE_R(R_SECT));
    hw_wr8(IDE_ALT, 0x00);
    c = hw_rd8(IDE_R(R_NSECT));
    d = hw_rd8(IDE_R(R_SECT));
    if (a == 0x12 && b == 0x56 && c == 0x34 && d == 0x78) {
        amt_case("ide_lba48_hob", AMT_ST_PASS, 0);
        return;
    }
    amt_case_open("ide_lba48_hob", AMT_ST_FAIL);
    json_obj("expect");
    json_str("hob1", "12 56");
    json_str("hob0", "34 78");
    json_obj_end();
    json_obj("got");
    json_hexn("hob1_nsect", a, 2); json_hexn("hob1_sector", b, 2);
    json_hexn("hob0_nsect", c, 2); json_hexn("hob0_sector", d, 2);
    json_obj_end();
    json_str("why", "48-bit task file: device control HOB selects the previous content of each register (ATA/ATAPI-6)");
    amt_case_close();
}

static void ide_body(void)
{
    t_ide_taskfile();
    t_ide_status();
    t_ide_identify();
    t_ide_hob();
}

static void t_ide(void)
{
    UBYTE a, b;
    int i, code;
    /* CLI start (CONTRACT section 14, bootio NULL): scsi.device and the
     * filesystems on this port are live, so the task file is never touched
     * unless the run says unsafe (review H2) */
    if (!amt_bootio && !(amt_par.flags & AMT_F_UNSAFE)) {
        amt_case("ide_present", AMT_ST_SKIP,
                 "CLI start: the IDE port belongs to the running scsi.device and filesystems; the task file "
                 "tests run only from the bootblock or with the unsafe flag");
        return;
    }
    if (!ide_wait_notbusy(500)) {
        amt_case("ide_present", AMT_ST_SKIP, "IDE status stays busy (no device answering on the Gayle port)");
        return;
    }
    if (!ide_idle()) {
        amt_case("ide_present", AMT_ST_SKIP,
                 "IDE status DRQ = 1: a data transfer of another command is pending; the command block "
                 "registers must not be written (ATA-4 7.15.6)");
        return;
    }
    for (i = R_NSECT; i <= R_SEL; i++)
        ide_sv[i] = hw_rd8(IDE_R(i));
    hw_wr8(IDE_R(R_NSECT), 0x55);
    hw_wr8(IDE_R(R_SECT), 0xaa);
    a = hw_rd8(IDE_R(R_NSECT));
    b = hw_rd8(IDE_R(R_SECT));
    if (a != 0x55 || b != 0xaa) {
        ide_restore();
        amt_case("ide_present", AMT_ST_SKIP, "no IDE device on the Gayle port (the task file does not hold written values)");
        return;
    }
    amt_case("ide_present", AMT_ST_PASS, 0);
    code = amt_try(ide_body);
    ide_restore();
    hw_wr8(G_INT, g_int0);
    if (code) {
        amt_case_open("ide_aborted", AMT_ST_FAIL);
        json_str("why", "unexpected exception in the IDE tests");
        json_obj("got");
        json_int("vector", amt_last_exc.vector);
        json_hex("pc", amt_last_exc.pc);
        json_obj_end();
        amt_case_close();
    }
}

/* ------------------------------------------------------------ PCMCIA */
static UBYTE attr(ULONG i) { return hw_rd8(ATTR + 2 * i); }

static ULONG card_size;     /* from CISTPL_DEVICE, 0 if unknown */

static const char *const card_cases[] = {
    "pcmcia_cis", "pcmcia_cis_device", "pcmcia_owner", "pcmcia_cis_size", "pcmcia_common_rw",
    "pcmcia_status_change", 0
};

static void skip_cards(int from, int st, const char *why)
{
    int i;
    for (i = from; card_cases[i]; i++)
        amt_case(card_cases[i], st, why);
}

/* cases 2..4 only (pcmcia_status_change is decided separately) */
static void skip_common(int from, const char *why)
{
    int i;
    for (i = from; i <= 4; i++)
        amt_case(card_cases[i], AMT_ST_SKIP, why);
}

static int card_is_ram;

static int t_cis(void)
{
    char codes[3 * 24 + 1], *p = codes;
    ULONG i = 0;
    int n = 0, end = 0, first = -1, devoff = -1;
    while (i < 0x200 && n < 24) {
        UBYTE c = attr(i), l;
        if (c == 0xff) {
            end = 1;
            break;
        }
        if (c == 0x00) {          /* CISTPL_NULL */
            i++;
            continue;
        }
        l = attr(i + 1);
        if (first < 0)
            first = c;
        if (c == 0x01 && devoff < 0)
            devoff = (int)i + 2;
        p = mh_hex(p, c, 2);
        *p++ = ' ';
        n++;
        if (l == 0xff) {          /* link FF: end of chain */
            end = 1;
            break;
        }
        i += 2 + l;
    }
    *p = 0;
    amt_case_open("pcmcia_cis", (end && first == 0x01) ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!(end && first == 0x01)) {
        json_obj("expect");
        json_str("cis", "tuple chain starting with CISTPL_DEVICE (01), ending with CISTPL_END (FF) within 512 bytes");
        json_obj_end();
        json_obj("got");
        json_int("ended", end);
        json_hexn("first", (ULONG)(first & 0xff), 2);
        json_obj_end();
        json_str("why", "the card's attribute memory (even bytes at $A00000) does not hold a valid CIS (PC Card Standard, metaformat)");
    }
    json_obj("ctx");
    json_str("tuples", codes);
    json_obj_end();
    amt_case_close();

    /* CISTPL_DEVICE: device id byte (type b7-4, WPS b3, speed b2-0; speed 7 =
     * extended speed bytes follow), then the size byte: units-1 in b7-3,
     * unit code b2-0 (512 B << 2*code) */
    card_size = 0;
    if (devoff < 0) {
        amt_case("pcmcia_cis_device", AMT_ST_FAIL, "no CISTPL_DEVICE tuple");
        return 0;
    }
    {
        UBYTE id = attr((ULONG)devoff), sz;
        ULONG k = (ULONG)devoff + 1;
        if ((id & 7) == 7)
            while ((attr(k++) & 0x80) && k < (ULONG)devoff + 8)
                ;
        sz = attr(k);
        if ((id >> 4) == 0) {
            amt_case("pcmcia_cis_device", AMT_ST_SKIP, "device type 0 (no memory device): not a memory card");
            return 1;
        }
        if (sz == 0xff) {
            amt_case("pcmcia_cis_device", AMT_ST_FAIL, "CISTPL_DEVICE has no device info (size byte is FF)");
            return 0;
        }
        card_size = (ULONG)((sz >> 3) + 1) * (512UL << (2 * (sz & 7)));
        amt_case_open("pcmcia_cis_device", AMT_ST_PASS);
        json_obj("ctx");
        json_int("type", id >> 4);
        json_int("wps", (id >> 3) & 1);
        json_int("speed", id & 7);
        json_int("size", (long)card_size);
        json_obj_end();
        amt_case_close();
    }
    return 1;
}

/* CONTRACT section 11 signature block */
static int sig_ok(ULONG a, ULONG blk, UBYTE *uuid, ULONG *total, ULONG *first_free)
{
    static UBYTE b[512];
    ULONG i, crc, t;
    for (i = 0; i < 512; i++)
        b[i] = hw_rd8(a + i);
    if (b[0] != 'A' || b[1] != 'M' || b[2] != 'T' || b[3] != 'O' || b[4] != 'W' || b[5] != 'N' ||
        b[6] != '0' || b[7] != '1')
        return 0;
    crc = ((ULONG)b[508] << 24) | ((ULONG)b[509] << 16) | ((ULONG)b[510] << 8) | b[511];
    if (crc32_update(0, b, 508) != crc)
        return 0;
#define U32(o) (((ULONG)b[o] << 24) | ((ULONG)b[(o) + 1] << 16) | ((ULONG)b[(o) + 2] << 8) | b[(o) + 3])
    t = U32(28);
    if (U32(24) != blk || U32(32) != 512 || U32(40) != 4)
        return 0;
    if (blk == 0) {
        for (i = 0; i < 16; i++)
            uuid[i] = b[8 + i];
        *total = t;
        *first_free = U32(36);
        return 1;
    }
    if (!hw_owner_match(*total, *first_free, 512, 4,
                        t, U32(36), U32(32), U32(40)))
        return 0;
    for (i = 0; i < 16; i++)
        if (uuid[i] != b[8 + i])
            return 0;
    return 1;
#undef U32
}

static void t_common(int writable)
{
    UBYTE uuid[16];
    ULONG total = 0, ff = 0, lo, hi, n, i, x, bad = ~0UL;
    ULONG lim = card_size && card_size < COMMON_MAX ? card_size : COMMON_MAX;
    ULONG eb = 0, gb = 0, bw = 0;
    int pass;

    card_is_ram = mh_ram_overlaps(COMMON, COMMON + lim);
    if (card_is_ram) {
        skip_common(2, "the card's common memory is in the exec memory list (used as RAM): never written");
        return;
    }
    if (!sig_ok(COMMON, 0, uuid, &total, &ff)) {
        skip_common(2, "not test-owned (no CONTRACT section 11 signature in the first 512 bytes of common memory)");
        return;
    }
    if (!hw_owned_domain(total, ff, lim) ||
        !sig_ok(COMMON + (total - 1) * 512, total - 1, uuid, &total, &ff)) {
        amt_case_open("pcmcia_owner", AMT_ST_FAIL);
        json_str("why", "block 0 is signed but the last block (total-1) is not signed the same way: the card size or "
                        "address decoding of common memory is wrong; not written");
        json_obj("got");
        json_int("total", (long)total);
        json_int("cis_size", (long)card_size);
        json_obj_end();
        amt_case_close();
        skip_common(3, "not test-owned (see pcmcia_owner)");
        return;
    }
    amt_case_open("pcmcia_owner", AMT_ST_PASS);
    json_obj("ctx");
    json_int("total", (long)total);
    json_int("first_free", (long)ff);
    json_obj_end();
    amt_case_close();
    if (card_size)
        amt_check32("pcmcia_cis_size", "bytes", card_size, total * 512,
                    "the CISTPL_DEVICE size does not match the size of the medium signed in common memory");
    else
        amt_case("pcmcia_cis_size", AMT_ST_SKIP, "CIS has no memory size");

    if (!writable) {
        amt_case("pcmcia_common_rw", AMT_ST_SKIP, "card write-protected (status WR = 0)");
        return;
    }
    /* region [max(ff,2), total-1) blocks, at most 64 KB */
    lo = (ff > 2 ? ff : 2) * 512;
    hi = (total - 1) * 512;
    if (hi > lo + 65536)
        hi = lo + 65536;
    if (hi <= lo + 64) {
        amt_case("pcmcia_common_rw", AMT_ST_SKIP, "no writable region on the medium");
        return;
    }
    n = hi - lo;
    /* three passes, each written over the whole region and verified with
     * the same access width: longwords (seeded random), words, bytes */
    for (pass = 0; pass < 3 && bad == ~0UL; pass++) {
        ULONG step = pass == 0 ? 4 : pass == 1 ? 2 : 1;
        amt_srand(amt_par.seed * 0x9e3779b9UL + 0x1919 + pass);
        for (i = 0; i + step <= n; i += step) {
            x = amt_rand();
            if (step == 4) hw_wr32(COMMON + lo + i, x);
            else if (step == 2) hw_wr16(COMMON + lo + i, (UWORD)x);
            else hw_wr8(COMMON + lo + i, (UBYTE)x);
        }
        amt_srand(amt_par.seed * 0x9e3779b9UL + 0x1919 + pass);
        for (i = 0; i + step <= n; i += step) {
            ULONG e = amt_rand(), g;
            if (step == 4) g = hw_rd32(COMMON + lo + i);
            else if (step == 2) { g = hw_rd16(COMMON + lo + i); e &= 0xffff; }
            else { g = hw_rd8(COMMON + lo + i); e &= 0xff; }
            if (g != e) {
                bad = i;
                eb = e;
                gb = g;
                bw = step;
                break;
            }
            if ((i & 0x3fff) == 0)
                amt_watchdog_kick();
        }
    }
    if (bad == ~0UL) {
        UBYTE check_uuid[16];
        ULONG check_total = 0, check_ff = 0;
        int same = sig_ok(COMMON, 0, check_uuid, &check_total, &check_ff) &&
                   check_total == total && check_ff == ff;
        for (i = 0; same && i < 16; i++)
            same = check_uuid[i] == uuid[i];
        if (!same || !sig_ok(COMMON + (total - 1) * 512, total - 1, uuid, &total, &ff))
            bad = (total - 1) * 512 - lo;
    }
    if (bad == ~0UL) {
        amt_case("pcmcia_common_rw", AMT_ST_PASS, 0);
        return;
    }
    amt_case_open("pcmcia_common_rw", AMT_ST_FAIL);
    json_obj("expect");
    json_hexn("data", eb, (int)(bw ? bw * 2 : 8));
    json_obj_end();
    json_obj("got");
    json_hexn("data", gb, (int)(bw ? bw * 2 : 8));
    json_obj_end();
    json_str("why", "card common memory did not keep long/word/byte writes (or the end signature was overwritten: aliasing)");
    json_obj("in");
    json_hex("addr", COMMON + lo + bad);
    json_int("width", (long)bw);
    json_int("region_bytes", (long)n);
    json_obj_end();
    amt_case_close();
}

static void t_status_change(void)
{
    UBYTE irq = hw_rd8(G_IRQ), cs, ir;
    int inserted;
    ULONG t0;
    if (card_is_ram) {
        amt_case("pcmcia_status_change", AMT_ST_SKIP, "the card is system RAM: it is never ejected");
        return;
    }
    if (irq & 3) {
        amt_case("pcmcia_status_change", AMT_ST_SKIP, "IRQ RESET/BERR bits set: a card change would reset the machine or bus-error");
        return;
    }
    hw_wr8(G_IRQ, (UBYTE)(0xbc | (irq & 3)));   /* clear CCDET latch */
    if (!amt_request("pcmcia", "eject", "{\"op\":\"eject\"}", amt_secs(10))) {
        amt_case("pcmcia_status_change", AMT_ST_UNTESTABLE, "host did not act on request pcmcia");
        return;
    }
    t0 = amt_ticks();
    do {
        cs = hw_rd8(G_CS);
    } while ((cs & 0x40) && mh_ms_since(t0) < 3000);
    ir = hw_rd8(G_IRQ);
    inserted = amt_request("pcmcia", "insert", "{\"op\":\"insert\"}", amt_secs(10));
    t0 = amt_ticks();
    while (!(hw_rd8(G_CS) & 0x40) && mh_ms_since(t0) < 5000)
        amt_watchdog_kick();
    hw_wr8(G_IRQ, (UBYTE)(0xbc | (irq & 3)));
    if (!inserted || !(hw_rd8(G_CS) & 0x40))
        amt_case("pcmcia_status_change", AMT_ST_FAIL,
                 "card reinsertion was not acknowledged or presence was not restored within 5 seconds");
    else if (!(cs & 0x40) && (ir & 0x40))
        amt_case("pcmcia_status_change", AMT_ST_PASS, 0);
    else {
        amt_case_open("pcmcia_status_change", AMT_ST_FAIL);
        json_obj("expect");
        json_str("after_eject", "status b6 CCDET = 0, IRQ b6 latched = 1");
        json_obj_end();
        json_obj("got");
        json_hexn("cs", cs, 2);
        json_hexn("irq", ir, 2);
        json_obj_end();
        json_str("why", "removing the card must drop CCDET and latch the card-detect change in $DA9000");
        amt_case_close();
    }
}

static void t_pcmcia(void)
{
    int cis;
    if (!(g_cs0 & 0x40)) {
        skip_cards(0, AMT_ST_SKIP, "no PCMCIA card configured (status CCDET = 0)");
        return;
    }
    cis = t_cis();
    if (!cis) {
        skip_cards(2, AMT_ST_SKIP, "no valid CIS (see pcmcia_cis)");
        return;
    }
    t_common((g_cs0 & 0x08) != 0);
    t_status_change();
}

/* ------------------------------------------------------------ entry */
static void gayle_body(void)
{
    t_id();
    t_regs();
    t_ide();
    t_pcmcia();
}

void mh_gayle(void)
{
    int code;
    /* card.resource: in the ResourceList, or (Kickstart 3.1 with a RAM card
     * inserted at cold start) only as the name of the card's MemHeader */
    if (!mh_find_name(&SysBase->ResourceList, "card.resource") &&
        !mh_find_name(&SysBase->MemList, "card.resource")) {
        amt_case("gayle_present", AMT_ST_SKIP, "no Gayle: the Kickstart has no card.resource (A600/A1200 only)");
        return;
    }
    g_int0 = hw_rd8(G_INT);
    g_irq0 = hw_rd8(G_IRQ);
    g_cs0 = hw_rd8(G_CS);
    amt_case_open("gayle_present", AMT_ST_PASS);
    json_obj("ctx");
    json_int("ccdet", (g_cs0 >> 6) & 1);
    json_int("wr", (g_cs0 >> 3) & 1);
    json_obj_end();
    amt_case_close();
    code = amt_try(gayle_body);
    hw_wr8(G_INT, g_int0);
    if (code) {
        amt_case_open("gayle_aborted", AMT_ST_FAIL);
        json_str("why", "unexpected exception in the Gayle tests (interrupt enable restored)");
        json_obj("got");
        json_int("vector", amt_last_exc.vector);
        json_hex("pc", amt_last_exc.pc);
        json_obj_end();
        amt_case_close();
    }
}
