/* rtg module: shared declarations (graphics cards, register level).
 * See rtg.c for the overview and docs/cases/rtg.md for every case. */
#ifndef RTG_H
#define RTG_H

#include "mod.h"
#include <stddef.h>
void *memset(void *dst, int c, size_t n);

/* board kinds (one per emulated card family/bus layout) */
enum {
    K_NONE = 0,
    K_PICASSO2,      /* Village Tronic Picasso II/II+ (GD5426/5428), Z2 mem+regs */
    K_PICASSO4_Z2,   /* Picasso IV (GD5446), Z2 mem + 128 KB regs/flash */
    K_PICASSO4_Z3,   /* Picasso IV (GD5446), one 32 MB Z3 board */
    K_PICCOLO,       /* Piccolo/SD64/Spectrum (GD5426/5434/5428): mem + 64 KB regs */
    K_GRAFFITY_Z2,   /* Ateo Graffity (GD5428) Z2 mem + 128 KB regs */
    K_PIXEL64,       /* Ateo Pixel64 (GD5434) */
    K_CV64,          /* Phase 5 CyberVision 64 (S3 Trio64), Z3 */
    K_CV643D_Z2,     /* CyberVision 64/3D (S3 ViRGE), Z2 */
    K_CV643D_Z3,     /* CyberVision 64/3D (S3 ViRGE), Z3 */
    K_OMNIBUS,       /* ArMax oMniBus (ET4000AX / ET4000W32) */
    K_DOMINO,        /* X-Pert Domino (ET4000) */
    K_MERLIN,        /* X-Pert Merlin (ET4000W32), Z2 or Z3 mem + Z2 regs */
    K_A2410,         /* Commodore A2410 (TMS34010) */
    K_ZZ9000,        /* MNT ZZ9000 */
    K_UAEGFX,        /* UAE Zorro II/III RTG (uaegfx), driver traps only */
    K_OTHER          /* recognised, no register-level suite */
};

/* graphics chips */
enum { CH_NONE = 0, CH_CIRRUS, CH_S3TRIO, CH_S3VIRGE, CH_ET4000, CH_TMS34010, CH_ZZ9000 };

/* VRAM window byte-lane relation to the natural (big-endian, byte n =
 * VRAM byte n) window: CPU byte at offset o is VRAM byte o ^ mask */
struct rtg_win {
    volatile UBYTE *p;
    ULONG size;           /* bytes we may touch */
    UBYTE mask;           /* 0 natural, 1 bytes swapped in words, 3 in longs */
    const char *name;
};

struct rtg_card {
    const char *id;       /* case id prefix */
    const char *name;
    UWORD kind, chip;
    UWORD bus;            /* 2 or 3 */
    UWORD nids;
    UBYTE ids[4];         /* acceptable chip id values (Cirrus CR27 & 0xfc) */
    ULONG man, prod_mem, prod_reg;
    volatile UBYTE *mem;  /* memory board base (natural VRAM window) */
    ULONG memsize;        /* memory board size (autoconfig) */
    ULONG vram_min;       /* documented minimum VRAM of the card */
    ULONG vram_max;       /* largest VRAM the card can have (bytes we may walk) */
    volatile UBYTE *regs; /* register board base (0 if none) */
    ULONG regsize;
    volatile UBYTE *io;   /* VGA port 0 */
    UBYTE ioxor;          /* port p at io + (p ^ ioxor) */
    UBYTE irq;            /* Amiga interrupt level of the vblank interrupt, 0 none/unknown */
    UBYTE irq_inferred;   /* irq comes from the chip id, not from the board id (Picasso II+) */
    UBYTE nwin;
    UBYTE dac_bt482;      /* BT482 at I/O 0-0x1F (RS0/1 from A2/A3): Merlin */
    struct rtg_win win[3];/* win[0] = natural */
};

extern struct rtg_card *rtg_cur;

/* VGA port access */
static inline void vo(UWORD port, UBYTE v) { rtg_cur->io[port ^ rtg_cur->ioxor] = v; }
static inline UBYTE vi(UWORD port) { return rtg_cur->io[port ^ rtg_cur->ioxor]; }
static inline void ix_w(UWORD port, UBYTE i, UBYTE v) { vo(port, i); vo(port + 1, v); }
static inline UBYTE ix_r(UWORD port, UBYTE i) { vo(port, i); return vi(port + 1); }
/* palette DAC ports: VGA, or a BT482 at RS 0-3 = offsets 0,4,8,C */
#define DAC_WI (rtg_cur->dac_bt482 ? 0x00 : 0x3c8)
#define DAC_D  (rtg_cur->dac_bt482 ? 0x04 : 0x3c9)
#define DAC_M  (rtg_cur->dac_bt482 ? 0x08 : 0x3c6)
#define DAC_RI (rtg_cur->dac_bt482 ? 0x0c : 0x3c7)
#define SR_W(i, v) ix_w(0x3c4, (i), (v))
#define SR_R(i)    ix_r(0x3c4, (i))
#define GR_W(i, v) ix_w(0x3ce, (i), (v))
#define GR_R(i)    ix_r(0x3ce, (i))
#define CR_W(i, v) ix_w(0x3d4, (i), (v))
#define CR_R(i)    ix_r(0x3d4, (i))

/* rtg.c helpers */
const char *rtg_cid(const char *suffix);          /* "<card>_<suffix>" (static buffer) */
void rtg_pass(const char *suffix);
void rtg_skip(const char *suffix, const char *why);
void rtg_untestable(const char *suffix, const char *why);
int  rtg_chk(const char *suffix, const char *what, ULONG expect, ULONG got, const char *why);
void rtg_ctx_card(void);                         /* json "card" object */

struct regm { UBYTE idx, mask; };
/* index/data register file read-back (writes only bits in mask, keeps the
 * others; restores every register) */
int  rtg_regfile(const char *suffix, UWORD port, const struct regm *l, int n, const char *why);

/* VGA generic */
struct vga_state {
    UBYTE misc, pel, arx, feat;
    UBYTE sr[0x20], gr[0x40], cr[0x80], ar[0x15];
    UBYTE dac[768];
};
void vga_save(struct vga_state *s, int nsr, int ngr, int ncr);
void vga_restore_std(const struct vga_state *s);  /* standard VGA regs + DAC */
void vga_color_io(void);                          /* MISC bit 0 = 1 (3Dx CRTC) */
void vga_std_tests(void);
void vga_set_640x480(void);                       /* standard 640x480 timings, 8 bit graphics */
int  vga_check_640x480(const char *suffix);
void vga_retrace_test(void);
void vga_palette_load(void);

/* VRAM generic */
void rtg_vram_tests(void);
void rtg_draw_test(const char *suffix, ULONG pitch, int lines);
int  rtg_screenshot(const char *suffix, const char *tag);

/* VBL interrupt (CR11 bits 4/5, input status 0 bit 7), optionally at Amiga level */
void vga_vint_test(void (*board_enable)(int on));

/* work area save (first 64 KB of VRAM) */
#define RTG_WORK  65536UL
extern UBYTE *rtg_save;     /* RTG_WORK bytes in the any pool */
extern UBYTE *rtg_buf;      /* 3 x 4 KB oracle buffers */

/* chip suites */
void rtg_cirrus(void);
void rtg_s3(void);
void rtg_et4000(void);
void rtg_a2410(void);
void rtg_zz9000(void);

/* board-level monitor switch / interrupt enable (rtg.c) */
void rtg_monswitch(int rtg);
void rtg_board_irq(int on);

#endif
