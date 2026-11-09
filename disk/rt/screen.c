/* amitorture status screen: what is running, on the Amiga's own display.
 *
 * Booting a test disk on a real Amiga, an FPGA or an emulator window shows:
 * the disk (n of N, its group), the machine as probed, one line per module
 * with its pass/fail/skip counts and state, the totals, and at the end the
 * verdict and what to do next. Background colour: blue while running, green
 * when finished without failures, red with failures.
 *
 * 40 x 25 characters: one lores bitplane of 320 x 200 (8000 bytes of chip
 * RAM), Kickstart's ROM topaz 8 font (OpenFont, so no font is shipped). The
 * glyphs are copied into a table at start, so drawing needs no OS.
 *
 * One display: the runtime's own copper list (`cop`), self-contained
 * (DIW/DDF/BPLCON/BPL1PT/colours, sprites disarmed); colours are changed by
 * patching its COLOR00 move.
 *   - OS phase (exec alive; also when started from a CLI with Intuition
 *     running): a VERTB interrupt server at priority -120, after
 *     graphics.library's own, sets COP1LC to our list and DMAEN|BPLEN|COPEN
 *     every frame. A graphics.library View was tried first: AROS does not
 *     show an old-style LoadView View, and the KS 1.3 strap leaves raster
 *     DMA off. The server wins on every Kickstart and on AROS because it is
 *     the last writer before the next frame. Removed again by screen_close
 *     (end_action=return), which restores the OS display.
 *   - bare phase (the runtime owns the interrupts): before every module
 *     that does not own the display, COP1LC points at it and DMACON has only
 *     DMAEN|BPLEN|COPEN set. A lores 1-plane display uses the odd DMA slots
 *     (no contention with a 68000) and the copper list is about 30 moves at
 *     the top of the frame. Modules flagged AMT_MF_OWNS_DISPLAY (display,
 *     copper, blitter, DMA contention, timing, interrupts, Paula) get the
 *     hardware to themselves: the screen is off (plain background colour)
 *     while they run, and says so beforehand.
 * The run parameter flag `noscreen` (AMT_F_NOSCREEN) turns all of it off:
 * every bare module then starts with all DMA off, as before the screen
 * existed.
 *
 * The screen is set up after the test pools and the bare stack are
 * allocated: on a 512 KB machine it never takes memory a test needs; if its
 * 8 KB of chip RAM is not there, the disk runs without it.
 */
#include <proto/exec.h>
#include <proto/graphics.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <graphics/text.h>
#include "rt.h"
#include "mod.h"
#include "screen.h"

struct GfxBase *GfxBase;

extern volatile ULONG amt_frames;                       /* rt/mod.c, counted by the bare VBL ISR */
extern const struct amt_module *const amt_modules[];    /* modlist.c */
extern const int amt_n_modules;

#define COLS 40
#define ROWS 25
#define W_BYTES 40
#define H_LINES 200
#define PLANE_BYTES (W_BYTES * H_LINES)

#define BG_RUN   0x0135
#define BG_PASS  0x0041
#define BG_FAIL  0x0700
#define FG       0x0fff

static int on;                      /* screen set up */
static UBYTE *plane;                /* bitplane (chip) */
static UWORD *cop;                  /* our copper list (chip) */
static UWORD *cop_bg, *cop_fg;      /* value words of its COLOR00/COLOR01 moves */
static UWORD bg = BG_RUN;
static UBYTE glyph[96][8];
static ULONG vb_cop;                /* is_Data of the VERTB server: copper list address */
static struct Interrupt vb_irq;
static int vb_added;
static UWORD os_dma;                /* DMACONR & (DMAEN|BPLEN|COPEN) at start */

/* VERTB server (OS phase): A1 = is_Data -> the copper list address.
 * COP1LC = our list, COPJMP1 strobed so it runs in this frame too (we are
 * in the vertical blank, before the display window: the frame after the
 * bare phase, which started from the OS's copinit, then shows our screen,
 * not one frame of stale pointers), DMACON SET DMAEN | BPLEN | COPEN.
 * The strobe only while the blitter is idle (DMACONR BBUSY = 0): a COPJMP
 * written by the CPU makes the copper's first fetch without allocating the
 * bus, so a running blit (trackdisk decodes MFM with the blitter) can take
 * the same DMA slot, and both pointers become old OR new: the copper then
 * runs from the blit's data and the blitter writes elsewhere. Real Agnus
 * does it, and Amiberry emulates it (custom.cpp, "RGA conflict"). During a
 * blit the copper starts our list at the next vertical blank by itself.
 * Returns with D0 = 0 (Z set) so the chain goes on. */
__asm__(
"	.text\n"
"	.even\n"
"_amt_scr_vbserver:\n"
"	move.l	(a1),d0\n"
"	beq.s	1f\n"
"	move.l	d0,0xdff080\n"
"	btst	#6,0xdff002\n"          /* DMACONR bit 14 BBUSY */
"	bne.s	2f\n"
"	move.w	d0,0xdff088\n"
"2:	move.w	#0x8380,0xdff096\n"
"1:	moveq	#0,d0\n"
"	rts\n");
extern void amt_scr_vbserver(void);

/* rows */
#define R_TITLE   0
#define R_BUILD   1
#define R_MACH1   3
#define R_MACH2   4
#define R_MACH3   5
#define R_HEAD    7
#define R_MOD0    8
#define MAX_MODS  10                /* rows 8-17; totals, then 4 message lines up to row 24 */
static int r_total, r_msg;

static const struct amt_module *mods[MAX_MODS];
static int n_mods;
static int disk_n, disk_total;
static char disk_group[16];
static ULONG run_frames0;
static int clock_started;

/* ------------------------------------------------------------ drawing */
static void font_load(struct TextFont *f)
{
    int c, y, i;
    const ULONG *loc = (const ULONG *)f->tf_CharLoc;
    const UBYTE *data = (const UBYTE *)f->tf_CharData;
    int ys = f->tf_YSize < 8 ? f->tf_YSize : 8;
    for (c = 32; c < 128; c++) {
        int idx = (c >= f->tf_LoChar && c <= f->tf_HiChar) ? c - f->tf_LoChar : f->tf_HiChar - f->tf_LoChar + 1;
        UWORD off = (UWORD)(loc[idx] >> 16), w = (UWORD)loc[idx];
        if (w > 8)
            w = 8;
        for (y = 0; y < ys; y++) {
            const UBYTE *row = data + y * f->tf_Modulo;
            UBYTE out = 0;
            for (i = 0; i < w; i++) {
                UWORD b = (UWORD)(off + i);
                if (row[b >> 3] & (0x80 >> (b & 7)))
                    out |= (UBYTE)(0x80 >> i);
            }
            glyph[c - 32][y] = out;
        }
    }
}

static void put_ch(int col, int row, char ch)
{
    UBYTE *p;
    const UBYTE *g;
    int y;
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS)
        return;
    if ((UBYTE)ch < 32 || (UBYTE)ch > 127)
        ch = '?';
    g = glyph[(UBYTE)ch - 32];
    p = plane + row * 8 * W_BYTES + col;
    for (y = 0; y < 8; y++, p += W_BYTES)
        *p = g[y];
}

/* text at (col,row); returns the next column */
static int put_str(int col, int row, const char *s)
{
    while (*s && col < COLS)
        put_ch(col++, row, *s++);
    return col;
}

static void clear_row(int row)
{
    int col;
    for (col = 0; col < COLS; col++)
        put_ch(col, row, ' ');
}

static void line_row(int row)
{
    int col;
    for (col = 0; col < COLS; col++)
        put_ch(col, row, '-');
}

/* right-aligned unsigned number in `width` columns ending before col+width */
static void put_num(int col, int row, ULONG v, int width)
{
    char buf[12];
    int n = (int)(amt_utoa(buf, v) - buf);
    int i;
    for (i = 0; i < width - n; i++)
        put_ch(col + i, row, ' ');
    put_str(col + (width > n ? width - n : 0), row, buf);
}

static void set_colours(UWORD b)
{
    bg = b;
    if (cop_bg)
        *cop_bg = b;
}

/* ------------------------------------------------------------ content */
static void parse_build(char *rest, int restlen)
{
    /* build id: "<rev> <date> [<n>/<N> ]<group>" (disk/Makefile); a single
     * ADF has just "<rev> <date>" */
    const char *b = amt_hdr->build_id;
    int len = 0, i, words = 0, ws[8], we[8], inword = 0;
    while (len < 64 && b[len])
        len++;
    for (i = 0; i <= len && words < 8; i++) {
        int sp = i == len || b[i] == ' ';
        if (!sp && !inword) {
            ws[words] = i;
            inword = 1;
        } else if (sp && inword) {
            we[words++] = i;
            inword = 0;
        }
    }
    /* the rest line: the whole id up to the n/N word */
    i = 0;
    if (words >= 4) {
        const char *w = b + ws[words - 2];
        int n = 0, t = 0, k = 0, ok = 1;
        while (w + k < b + we[words - 2] && w[k] != '/') {
            if (w[k] < '0' || w[k] > '9') ok = 0;
            n = n * 10 + (w[k++] - '0');
        }
        if (w[k] == '/') {
            k++;
            while (w + k < b + we[words - 2]) {
                if (w[k] < '0' || w[k] > '9') ok = 0;
                t = t * 10 + (w[k++] - '0');
            }
        } else
            ok = 0;
        if (ok && n > 0 && t >= n) {
            int g = 0;
            disk_n = n;
            disk_total = t;
            for (k = ws[words - 1]; k < we[words - 1] && g < (int)sizeof disk_group - 1; k++)
                disk_group[g++] = b[k];
            disk_group[g] = 0;
            len = ws[words - 2] > 0 ? ws[words - 2] - 1 : 0;
        }
    }
    for (i = 0; i < len && i < restlen - 1; i++)
        rest[i] = b[i];
    rest[i] = 0;
}

static ULONG mem_kb(ULONG attr_mask)
{
    LONG i;
    ULONG sum = 0;
    /* exec's region starts after its MemHeader: round the bounds to 4 KB */
    for (i = 0; i < amt_probe_result.n_mem; i++)
        if (amt_probe_result.mem[i].attr & attr_mask)
            sum += ((amt_probe_result.mem[i].upper + 0xfffUL) & ~0xfffUL) - (amt_probe_result.mem[i].lower & ~0xfffUL);
    return sum >> 10;
}

static void draw_static(void)
{
    char buf[48], *p;
    int col;
    const struct amt_probe *pr = &amt_probe_result;

    col = put_str(0, R_TITLE, "AMITORTURE");
    if (disk_n) {
        p = buf;
        p = amt_utoa(p, (ULONG)disk_n);
        *p++ = ' '; *p++ = 'o'; *p++ = 'f'; *p++ = ' ';
        p = amt_utoa(p, (ULONG)disk_total);
        *p = 0;
        col = put_str(col + 2, R_TITLE, "disk ");
        col = put_str(col, R_TITLE, buf);
        col = put_str(col, R_TITLE, ": ");
        put_str(col, R_TITLE, disk_group);
    } else
        put_str(col + 2, R_TITLE, "test disk");
    parse_build(buf, sizeof buf);
    col = put_str(0, R_BUILD, "build ");
    put_str(col, R_BUILD, buf);
    line_row(R_BUILD + 1);

    col = put_str(0, R_MACH1, "CPU ");
    col = put_str(col, R_MACH1, pr->cpu ? pr->cpu : "?");
    col = put_str(col + 2, R_MACH1, "FPU ");
    col = put_str(col, R_MACH1, pr->fpu ? pr->fpu : "?");
    col = put_str(col + 2, R_MACH1, "MMU ");
    put_str(col, R_MACH1, pr->mmu ? "yes" : "no");

    col = put_str(0, R_MACH2, pr->chipset ? pr->chipset : "?");
    col = put_str(col + 1, R_MACH2, pr->pal ? "PAL" : "NTSC");
    col = put_str(col + 2, R_MACH2, "Kickstart ");
    amt_vstr(buf, pr->kick_ver, pr->kick_rev);
    put_str(col, R_MACH2, buf);

    col = put_str(0, R_MACH3, "chip ");
    p = amt_utoa(buf, mem_kb(MEMF_CHIP));
    *p++ = 'K';
    *p = 0;
    col = put_str(col, R_MACH3, buf);
    col = put_str(col + 2, R_MACH3, "fast ");
    p = amt_utoa(buf, mem_kb(MEMF_FAST));
    *p++ = 'K';
    *p = 0;
    col = put_str(col, R_MACH3, buf);
    col = put_str(col + 2, R_MACH3, "boards ");
    put_num(col, R_MACH3, (ULONG)pr->n_boards, 1);
    line_row(R_MACH3 + 1);

    put_str(0, R_HEAD, "MODULE          PASS FAIL SKIP N/T");
    line_row(r_total - 1);
}

static int mod_row(const struct amt_module *m)
{
    int i;
    for (i = 0; i < n_mods; i++)
        if (mods[i] == m)
            return R_MOD0 + i;
    return -1;
}

static void draw_counts(int row, ULONG pass, ULONG fail, ULONG skip, ULONG unt)
{
    put_num(14, row, pass, 6);
    put_num(20, row, fail, 5);
    put_num(25, row, skip, 5);
    put_num(30, row, unt, 4);
}

static void draw_state(int row, const char *st)
{
    int col = put_str(35, row, st);
    while (col < COLS)
        put_ch(col++, row, ' ');
}

static void draw_time(void)
{
    /* elapsed time of the bare phase clock (amt_frames counts VBLs there) */
    char buf[16], *p;
    ULONG hz = amt_probe_result.pal ? 50 : 60;
    ULONG s;
    int n, col;
    if (!clock_started)
        return;
    s = (amt_frames - run_frames0) / hz;
    p = amt_utoa(buf, s / 60 > 99 ? 99 : s / 60);
    *p++ = ':';
    *p++ = (char)('0' + (s % 60) / 10);
    *p++ = (char)('0' + s % 10);
    *p = 0;
    n = (int)(p - buf);
    for (col = 35; col < COLS - n; col++)
        put_ch(col, r_total, ' ');
    put_str(COLS - n, r_total, buf);
}

static void msg(int line, const char *a, const char *b, const char *c)
{
    int row = r_msg + line, col;
    clear_row(row);
    col = put_str(0, row, a);
    if (b)
        col = put_str(col, row, b);
    if (c)
        put_str(col, row, c);
}

/* ------------------------------------------------------------ copper */
static UWORD *cmove(UWORD *c, UWORD reg, UWORD val, UWORD **valp)
{
    *c++ = reg;
    if (valp)
        *valp = c;
    *c++ = val;
    return c;
}

static void build_copper(void)
{
    UWORD *c = cop;
    UWORD vstart = amt_probe_result.pal ? 0x48 : 0x2c;   /* 200 lines, centred on PAL */
    UWORD vstop = (UWORD)((vstart + H_LINES) & 0xff);
    int aga = amt_probe_result.chipset && amt_probe_result.chipset[0] == 'A';
    int i;
    c = cmove(c, 0x08e, (UWORD)((vstart << 8) | 0x81), 0);  /* DIWSTRT */
    c = cmove(c, 0x090, (UWORD)((vstop << 8) | 0xc1), 0);   /* DIWSTOP */
    c = cmove(c, 0x092, 0x0038, 0);                         /* DDFSTRT */
    c = cmove(c, 0x094, 0x00d0, 0);                         /* DDFSTOP */
    c = cmove(c, 0x100, 0x1200, 0);                         /* BPLCON0: 1 plane, COLOR */
    c = cmove(c, 0x102, 0x0000, 0);                         /* BPLCON1 */
    c = cmove(c, 0x104, 0x0024, 0);                         /* BPLCON2 */
    if (aga) {
        c = cmove(c, 0x106, 0x0c00, 0);                     /* BPLCON3 (AGA default) */
        c = cmove(c, 0x10c, 0x0011, 0);                     /* BPLCON4 (AGA default) */
        c = cmove(c, 0x1fc, 0x0000, 0);                     /* FMODE */
    }
    c = cmove(c, 0x108, 0x0000, 0);                         /* BPL1MOD */
    c = cmove(c, 0x10a, 0x0000, 0);                         /* BPL2MOD */
    for (i = 0; i < 8; i++)
        c = cmove(c, (UWORD)(0x142 + 8 * i), 0x0000, 0);    /* SPRxCTL: disarmed */
    c = cmove(c, 0x0e0, (UWORD)((ULONG)plane >> 16), 0);    /* BPL1PTH */
    c = cmove(c, 0x0e2, (UWORD)(ULONG)plane, 0);            /* BPL1PTL */
    c = cmove(c, 0x180, bg, &cop_bg);                       /* COLOR00 */
    c = cmove(c, 0x182, FG, &cop_fg);                       /* COLOR01 */
    *c++ = 0xffff;                                          /* wait forever */
    *c++ = 0xfffe;
}

#define COP_WORDS 64

/* ------------------------------------------------------------ OS phase */
static void os_display_on(void)
{
    os_dma = CREG(R_DMACONR) & 0x0380;
    vb_cop = (ULONG)cop;
    vb_irq.is_Node.ln_Type = NT_INTERRUPT;
    vb_irq.is_Node.ln_Pri = -120;
    vb_irq.is_Node.ln_Name = (char *)"amitorture screen";
    vb_irq.is_Data = &vb_cop;
    vb_irq.is_Code = amt_scr_vbserver;
    AddIntServer(INTB_VERTB, &vb_irq);
    vb_added = 1;
}

/* ------------------------------------------------------------ API */
void screen_init(void)
{
    static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, FPF_ROMFONT };
    struct TextFont *f;
    char tmp[48];
    int i;

    if (amt_par.flags & AMT_F_NOSCREEN)
        return;
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 0);
    if (!GfxBase)
        return;
    f = OpenFont(&topaz8);
    if (!f)
        return;
    font_load(f);
    CloseFont(f);
    plane = AllocMem(PLANE_BYTES + COP_WORDS * 2, MEMF_CHIP | MEMF_CLEAR);
    if (!plane)
        return;
    cop = (UWORD *)(plane + PLANE_BYTES);

    n_mods = 0;
    for (i = 0; i < amt_n_modules && n_mods < MAX_MODS; i++)
        if (amt_modules[i]->phase == AMT_PH_OS && amt_module_enabled(amt_modules[i]))
            mods[n_mods++] = amt_modules[i];
    for (i = 0; i < amt_n_modules && n_mods < MAX_MODS; i++)
        if (amt_modules[i]->phase == AMT_PH_BARE && amt_module_enabled(amt_modules[i]))
            mods[n_mods++] = amt_modules[i];
    r_total = R_MOD0 + n_mods + 1;
    r_msg = r_total + 2;

    parse_build(tmp, sizeof tmp);
    on = 1;
    draw_static();
    for (i = 0; i < n_mods; i++) {
        put_str(0, R_MOD0 + i, mods[i]->name);
        draw_counts(R_MOD0 + i, 0, 0, 0, 0);
        draw_state(R_MOD0 + i, "-");
    }
    put_str(0, r_total, "TOTAL");
    draw_counts(r_total, 0, 0, 0, 0);
    msg(0, "Running. Results go to the serial port", 0, 0);
    if (amt_par.flags & AMT_F_NOSERIAL)
        msg(1, "(serial output is off for this run)", 0, 0);
    else {
        char b[16];
        amt_utoa(b, amt_par.baud);
        msg(1, "at ", b, " baud, 8N1, no handshake.");
    }
    build_copper();
    os_display_on();
}

void screen_module(const struct amt_module *m, int begin, ULONG pass, ULONG fail, ULONG skip, ULONG unt,
                   int aborted)
{
    int row;
    if (!on)
        return;
    row = mod_row(m);
    if (row < 0)
        return;
    if (begin) {
        draw_state(row, (m->flags & AMT_MF_OWNS_DISPLAY) && m->phase == AMT_PH_BARE ? "run*" : "run");
        if ((m->flags & AMT_MF_OWNS_DISPLAY) && m->phase == AMT_PH_BARE)
            msg(2, "* ", m->name, ": display hardware test,");
        else
            clear_row(r_msg + 2);
        if ((m->flags & AMT_MF_OWNS_DISPLAY) && m->phase == AMT_PH_BARE)
            msg(3, "  the screen is blank while it runs.", 0, 0);
        else
            clear_row(r_msg + 3);
    } else {
        draw_counts(row, pass, fail, skip, unt);
        draw_state(row, aborted ? "ABRT" : fail ? "FAIL" : (pass || unt) ? "ok" : skip ? "skip" : "done");
    }
    draw_time();
}

void screen_count(const struct amt_module *m, ULONG pass, ULONG fail, ULONG skip, ULONG unt,
                  ULONG tpass, ULONG tfail, ULONG tskip, ULONG tunt)
{
    int row;
    if (!on)
        return;
    row = mod_row(m);
    if (row >= 0)
        draw_counts(row, pass, fail, skip, unt);
    draw_counts(r_total, tpass, tfail, tskip, tunt);
    draw_time();
}

void screen_bench(int line, const char *a, const char *b, const char *c)
{
    if (!on || line < 0 || r_msg + 5 + line >= ROWS)
        return;
    msg(5 + line, a, b, c);
    draw_time();
}

void screen_bare(int display_on)
{
    if (!on)
        return;
    if (!clock_started) {               /* first bare module: the VBL clock starts */
        clock_started = 1;
        run_frames0 = amt_frames;
    }
    if (display_on) {
        CREG(0x080) = (UWORD)((ULONG)cop >> 16);    /* COP1LC */
        CREG(0x082) = (UWORD)(ULONG)cop;
        /* no CPU COPJMP1 while a blit runs (see amt_scr_vbserver); the
         * first read after BLTSIZE can miss BBUSY on old Agnus: read twice */
        (void)CREG(R_DMACONR);
        while (CREG(R_DMACONR) & 0x4000)
            ;
        CREG(0x088) = 0;                            /* COPJMP1 */
        CREG(R_DMACON) = 0x8380;                    /* SET | DMAEN | BPLEN | COPEN */
    } else {
        CREG(R_DMACON) = 0x0180;                    /* BPLEN | COPEN off */
        CREG(0x180) = bg;                           /* plain background */
    }
}

void screen_finish(ULONG fail, ULONG aborted)
{
    char b[12];
    if (!on)
        return;
    set_colours(fail || aborted ? BG_FAIL : BG_PASS);
    clear_row(r_msg + 2);
    clear_row(r_msg + 3);
    if (amt_par.flags & AMT_F_NOSERIAL)
        msg(1, "Serial output was off for this run.", 0, 0);
    else {
        char bd[12];
        amt_utoa(bd, amt_par.baud);
        msg(1, "Results: serial port, ", bd, " baud 8N1.");
    }
    if (fail || aborted) {
        amt_utoa(b, fail);
        msg(0, "FINISHED: ", b, aborted ? " failed, modules aborted." : " failed case(s).");
    } else
        msg(0, "FINISHED: no failures.", 0, 0);
    if (disk_n && disk_n < disk_total) {
        char n[8];
        amt_utoa(n, (ULONG)disk_n + 1);
        msg(2, "Next: insert disk ", n, ", then reset.");
    } else if (disk_n)
        msg(2, "That was the last disk of the set.", 0, 0);
    if (amt_par.end_action == AMT_END_HALT)
        msg(3, "Halted. Reset to continue.", 0, 0);
    else if (amt_par.end_action == AMT_END_REBOOT)
        msg(3, "Rebooting...", 0, 0);
    else if (amt_par.end_action == AMT_END_RETURN)
        msg(3, "Returning to Kickstart...", 0, 0);
    /* return / reboot: keep the verdict on screen for a moment, then the
     * caller closes it or reboots (halt keeps it up for good) */
    if (amt_par.end_action != AMT_END_HALT && GfxBase) {
        int k;
        for (k = 0; k < 150; k++)
            WaitTOF();
    }
}

void screen_close(void)
{
    if (!on)
        return;
    if (vb_added) {
        RemIntServer(INTB_VERTB, &vb_irq);
        vb_added = 0;
    }
    vb_cop = 0;
    /* the OS display back: graphics' copinit list (which jumps to the
     * active View's list), the View itself, and the raster/copper DMA bits
     * as they were */
    if (GfxBase) {
        if (GfxBase->copinit) {
            CREG(0x080) = (UWORD)((ULONG)GfxBase->copinit >> 16);
            CREG(0x082) = (UWORD)(ULONG)GfxBase->copinit;
        }
        if (GfxBase->ActiView)
            LoadView(GfxBase->ActiView);
    }
    CREG(R_DMACON) = (UWORD)(0x0180 & ~os_dma);     /* clear the bits that were off */
    CREG(0x180) = 0;            /* our background must not linger (KS 1.3 has no View to reload it) */
    if (GfxBase) {
        WaitTOF();
        WaitTOF();
    }
    FreeMem(plane, PLANE_BYTES + COP_WORDS * 2);
    plane = 0;
    cop = cop_bg = cop_fg = 0;
    on = 0;
}
