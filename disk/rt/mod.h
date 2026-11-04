/* amitorture test-module framework (CONTRACT.md section 10).
 *
 * A module is one C file in disk/modules/ (plus optional asm/CPU-specific
 * objects) that declares itself with exactly one line at column 0:
 *
 *     AMT_MODULE(cia, 7, AMT_PH_BARE, 0, 16384, 0);
 *
 * The Makefile greps those lines and generates the module table, so modules
 * never edit a shared file. Arguments:
 *     name    C identifier, also the "mod" value in records
 *     id      unique number 0-63 (bit in the run parameter module mask);
 *             ids are assigned in CONTRACT.md section 10
 *     phase   AMT_PH_OS   - runs with exec alive, multitasking, before takeover
 *             AMT_PH_BARE - runs after takeover: supervisor mode, own stack,
 *                           INTENA = INTEN|VERTB (the runtime's VBL);
 *                           DMA: DMAEN|BPLEN|COPEN for the status screen
 *                           (rt/screen.c), or all off for modules flagged
 *                           AMT_MF_OWNS_DISPLAY and with the noscreen flag
 *     flags   AMT_MF_*
 *     chip    bytes of chip RAM the module needs (amt_chip_pool())
 *     any     bytes of any RAM (fast if present) it needs (amt_any_pool())
 * The function `void amt_run_<name>(void)` is the module body.
 *
 * Every test result is reported with amt_case*(); the runtime emits the
 * "mod" begin/end records, counts pass/fail/skip/untestable, and turns an
 * unexpected exception or a watchdog expiry into an "aborted" case.
 *
 * The rules of the project apply to every module (see docs/plan):
 *   - deterministic: same seed and same CPU/FPU variant -> same records, so
 *     JIT on/off and different emulators can be compared case by case;
 *   - expectations come from the detected variant's architecture rules
 *     (amt_probe_result), never from "what emulator X does";
 *   - only touch memory from the pools or your own stack/BSS, only touch
 *     hardware the module tests, and leave it as the runtime handed it over;
 *   - every failing case carries expect/got/why and enough inputs to
 *     reproduce it without the disk.
 */
#ifndef AMT_MOD_H
#define AMT_MOD_H

#include <exec/types.h>
#include "rt.h"
#include "../probe/probe.h"

/* ---- declaration ---- */
#define AMT_PH_OS    0
#define AMT_PH_BARE  1

#define AMT_MF_LONG       0x0002  /* watchdog limit x10 */
#define AMT_MF_NEEDS_FPU  0x0004  /* skip whole module if no FPU (one skip case) */
#define AMT_MF_OWNS_DISPLAY 0x0008 /* tests display/DMA/beam timing: the status screen (bitplane and
                                     copper DMA) is off while it runs (rt/screen.c) */

struct amt_module {
    const char *name;
    UBYTE id;
    UBYTE phase;
    UWORD flags;
    ULONG chip;
    ULONG any;
    void (*run)(void);
};

#define AMT_MODULE(name, id, phase, flags, chip, any) \
    void amt_run_##name(void); \
    const struct amt_module amt_mod_##name = \
        { #name, (id), (phase), (flags), (chip), (any), amt_run_##name }

/* ---- environment ---- */
extern struct amt_probe amt_probe_result;   /* filled before any module runs */
#define AMT_CPU()   (amt_probe_result.cpu_id)       /* AMT_CPU_* */
#define AMT_FPU()   (amt_probe_result.fpu_id)       /* AMT_FPU_* (0 = none) */
#define AMT_MMU()   (amt_probe_result.mmu)
#define AMT_QUICK() (amt_par.mode == AMT_MODE_QUICK)

void *amt_chip_pool(void);       /* 16-byte aligned, at least `chip` bytes */
void *amt_any_pool(void);        /* 16-byte aligned, at least `any` bytes */
int   amt_any_pool_is_chip(void);/* 1 if the any-pool had to come from chip RAM
                                    (no fast/slow RAM). Note: Amiga chip RAM does
                                    not support the 68k indivisible read-modify-
                                    write cycle (TAS/CAS/CAS2): the write may be
                                    lost (HRM); don't test RMW there. */

/* ---- expansion boards ----
 * Every module that touches a Zorro board's registers or memory MUST get the
 * board from amt_probe_result.boards[] AND pass it through amt_board_ok()
 * first. It rejects boards that are shut up (cd_Flags CDF_SHUTUP; CDF_CONFIGME
 * only means "no driver has claimed it yet" and is accepted), have
 * no address or size, or don't lie entirely inside a Zorro II window
 * ($200000-$9FFFFF memory, $E90000-$EFFFFF I/O; $E8xxxx is the autoconfig slot) or Zorro III space
 * (>= $10000000), or wrap around the address space. Writing to a rejected
 * board's "address" could hit chip RAM (e.g. address 0) or a board that is
 * still in configuration space. Returns 1 if the board may be accessed. */
int amt_board_ok(const struct amt_probe_board *b);

/* ---- results ---- */
#define AMT_ST_PASS        0
#define AMT_ST_FAIL        1
#define AMT_ST_SKIP        2
#define AMT_ST_UNTESTABLE  3

/* Simple form: one complete record. `why` is required for FAIL/SKIP/
 * UNTESTABLE (the reason), ignored for PASS (may be NULL). */
void amt_case(const char *case_id, int st, const char *why);

/* Rich form: amt_case_open() starts the record (mod, case, st); the caller
 * then adds fields with the json_*() helpers, e.g.
 *     json_obj("expect"); json_hex("d0", e); json_obj_end();
 *     json_obj("got");    json_hex("d0", g); json_obj_end();
 *     json_str("why", "ADD.L overflow: V clear");
 *     json_obj("in");  ... inputs ... json_obj_end();
 *     json_obj("ctx"); ... opcode words, sr, pc ... json_obj_end();
 * and finishes with amt_case_close(). Required keys for a failure:
 * expect, got, why. */
void amt_case_open(const char *case_id, int st);
void amt_case_close(void);

/* Compare helper: one 32-bit value. Emits a pass, or a fail with
 * expect/got {"<what>": hex} and why. Returns 1 on pass. */
int amt_check32(const char *case_id, const char *what, ULONG expect, ULONG got,
                const char *why);

/* Ask the host to capture output now (best effort; it may not act). */
void amt_capture(const char *what, const char *tag);

/* Host request channel (CONTRACT.md section 13). Emits a "request" record
 * {id, what, tag, args} and waits until the host writes that id into the
 * mailbox host_ack field (it does so after acting: screenshot saved, audio
 * mark set, key/mouse event injected...). Returns 1 when acknowledged, 0 on
 * timeout (no host that understands requests: e.g. a serial-only capture on
 * real hardware). A module must then report the dependent case
 * AMT_ST_UNTESTABLE with why "host did not act on request <what>".
 * `args_json` is a JSON object text or NULL. Works in both phases; kicks
 * the watchdog while waiting. Typical timeout: 10 s (amt_secs(10)). */
int amt_request(const char *what, const char *tag, const char *args_json, ULONG timeout_frames);
ULONG amt_secs(ULONG s);          /* seconds -> frames (50 PAL / 60 NTSC) */

/* Throughput record for the current module. */
/* ticks in colour clocks (amt_ticks() deltas) */
void amt_perf(const char *name, ULONG ops, ULONG ticks);
/* ticks in any unit: give its frequency and a short unit name
 * ("eclock", "cck", "us"...) */
void amt_perf_hz(const char *name, ULONG ops, ULONG ticks, ULONG hz, const char *unit);

/* ---- bare-phase services (AMT_PH_BARE modules only) ----
 * The runtime owns the level-3 VERTB interrupt: frame counter, heartbeat,
 * watchdog. No CIA is touched by the runtime. Interrupts are enabled at
 * SR=$2000 with INTENA = INTEN|VERTB. A module that needs other interrupt
 * sources sets INTENA itself and installs a C hook per level:
 *     amt_irq_hook[level] = fn;   void fn(ULONG pending)
 * `pending` is INTREQR & INTENAR restricted to that level's bits
 * (L1 $0007, L2 $0008, L3 $0070, L4 $0780, L5 $1800, L6 $2000).
 * The hook must clear the INTREQ bits it handles. Level 3 calls the hook
 * for COPER/BLIT (VERTB stays with the runtime). A module that must own
 * VERTB too installs vector 27 itself (the watchdog then stops). */
extern void (*volatile amt_irq_hook[8])(ULONG intreq);
ULONG amt_frames_now(void);     /* VBL count since takeover */
/* Consistent beam position (V << 8) | H, V = VPOSR bits 2-0 : VHPOSR bits
 * 15-8. Reads VPOSR, VHPOSR, VPOSR and retries until both VPOSR reads agree,
 * so a read never mixes the V8-V10 bits of one line with the V0-V7/H of
 * another (e.g. line 255 -> 256 read as line 0). */
static inline ULONG amt_beam(void)
{
    UWORD a, vh, b;
    do {
        a = *(volatile UWORD *)0xdff004;    /* VPOSR */
        vh = *(volatile UWORD *)0xdff006;   /* VHPOSR */
        b = *(volatile UWORD *)0xdff004;
    } while ((a ^ b) & 7);
    return ((ULONG)(a & 7) << 16) | vh;
}
ULONG amt_ticks(void);           /* colour clocks from the beam counter + VBL
                                    count (amt_cck_hz() per second) */
ULONG amt_cck_hz(void);          /* colour clock: 3546895 PAL, 3579545 NTSC */
ULONG amt_eclk_hz(void);         /* CIA E-clock = cck/5: 709379 PAL, 715909 NTSC */
#define amt_eclock_hz amt_cck_hz /* old name (it always meant the colour clock) */
void  amt_watchdog_kick(void);   /* restart the module's watchdog period */

/* Exception vectors (VBR-relative, 2..63). The runtime installs a catch-all
 * that aborts the module with a report; modules install their own and get
 * the previous one back. Everything is reset after each module. */
typedef void (*amt_vec_fn)(void);
amt_vec_fn amt_vec_set(int vec, amt_vec_fn handler);
volatile ULONG *amt_vbr(void);   /* the vector table in use */

/* Run fn and catch an abort. Returns 0 if fn returned normally, else the
 * abort code. Nestable (inner try restores the outer jump point). */
#define AMT_ABORT_UNEXPECTED 1    /* unexpected exception: see amt_last_exc */
#define AMT_ABORT_WATCHDOG   2
#define AMT_ABORT_USER       3
int  amt_try(void (*fn)(void));
void amt_abort(int code);         /* from C or from inside a handler */

struct amt_exc_info {
    UWORD vector;                 /* vector number that hit the catch-all */
    UWORD sr;                     /* stacked SR (68000 group 1/2 layout or 010+) */
    ULONG pc;                     /* stacked PC */
    UWORD frame[16];              /* raw words from the exception frame */
};
extern struct amt_exc_info amt_last_exc;

UWORD amt_get_sr(void);
void  amt_set_sr(UWORD sr);

/* ---- helpers ---- */
ULONG amt_rand(void);             /* xorshift32 seeded from the run seed per module */
void  amt_srand(ULONG seed);

/* Hot-loop repetition (the JIT/interpreter self-check of the plan):
 * calls fn(0) once and records its result, then calls fn(i) for i=1..reps
 * and compares each result with the first. Returns -1 if all match, else
 * the first diverging iteration (and *first / *bad hold the two values).
 * fn must be deterministic in its argument-independent part: it may use i
 * only for things that cancel out (or ignore it). */
long amt_hot(ULONG (*fn)(ULONG iter), ULONG reps, ULONG *first, ULONG *bad);

/* Standard repetition count: 100 in full mode, 20 in quick. Amiberry's JIT
 * translates a block after 4 executions, so 100 is a 25x margin. A module
 * whose hot pass walks its inputs (cpu_basic) sizes it by coverage. */
ULONG amt_hot_reps(void);

#endif
