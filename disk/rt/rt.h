/* amitorture runtime: shared declarations (CONTRACT.md sections 3-7) */
#ifndef AMT_RT_H
#define AMT_RT_H

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/io.h>

/* --- payload header (section 1) and run parameters (section 4) --- */
struct amt_params {
    char  magic[4];          /* "PARM" or zero (defaults) */
    ULONG mode;              /* 0 full, 1 quick, 2 soak */
    ULONG seed;
    ULONG end_action;        /* 0 halt, 1 ColdReboot, 2 return -1 */
    ULONG flags;
    ULONG baud;
    ULONG modmask[2];        /* [0] = bits 63..32, [1] = bits 31..0 */
    ULONG floppy;            /* drive speed the modules run at, percent; 0 = not stated
                                (100); AMT_FLOPPY_TURBO = a DMA is copied at once */
    ULONG reserved[7];
};

#define AMT_FLOPPY_TURBO 0xFFFFUL

#define AMT_MODE_FULL   0
#define AMT_MODE_QUICK  1
#define AMT_MODE_SOAK   2

#define AMT_END_HALT    0
#define AMT_END_REBOOT  1
#define AMT_END_RETURN  2

#define AMT_F_UNSAFE    1
#define AMT_F_NOSERIAL  2
#define AMT_F_NOMBOX    4
#define AMT_F_NOSCREEN  16   /* no status screen: bare modules start with all DMA off */
#define AMT_F_INTCOMPAT 32   /* integer instructions use an emulator compatibility policy */
#define AMT_F_FPUCOMPAT 64   /* FPU instructions use an emulator compatibility policy */
#define AMT_F_MODPACE   128  /* the runner sets pacing per module: a "pacing" request (tag = module
                                name) before each module (CONTRACT §13) */
#define AMT_F_NET       8    /* the host says the network cards are on an emulator-only
                                backend (user-mode NAT): the network module may transmit */

struct amt_header {
    char  magic[4];          /* "AMTP" */
    ULONG version;
    ULONG image_size;        /* code (segment 0) */
    ULONG bss_size;          /* after the data in segment 1 */
    ULONG reloc_count;
    ULONG entry_offset;
    ULONG blob_sectors;
    ULONG blob_crc32;
    ULONG alloc_size;        /* segment 0 */
    ULONG alloc_flags;
    ULONG data_size;         /* segment 1 stored bytes */
    ULONG data_sectors;
    ULONG data_alloc;        /* segment 1 */
    ULONG pad[3];
    struct amt_params params;    /* offset 64 */
    char  build_id[64];          /* offset 128 */
    UBYTE rest[512 - 192];
};

/* --- globals set by start.s --- */
extern struct ExecBase *SysBase;
extern const struct amt_header *amt_hdr;
extern struct IOStdReq *amt_bootio;
extern void *amt_base;        /* segment 0: code */
extern void *amt_base2;       /* segment 1: data + BSS */

/* effective run parameters (defaults filled in) */
extern struct amt_params amt_par;

/* --- custom chips --- */
#define CUSTOM_BASE 0xdff000UL
#define CREG(off) (*(volatile UWORD *)(CUSTOM_BASE + (off)))
#define R_DMACONR  0x002
#define R_VPOSR    0x004
#define R_VHPOSR   0x006
#define R_ADKCONR  0x010
#define R_SERDATR  0x018
#define R_INTENAR  0x01c
#define R_INTREQR  0x01e
#define R_SERDAT   0x030
#define R_SERPER   0x032
#define R_DMACON   0x096
#define R_INTENA   0x09a
#define R_INTREQ   0x09c
#define R_ADKCON   0x09e
#define R_COLOR00  0x180

/* --- crc32.c --- */
ULONG crc32_update(ULONG crc, const void *buf, ULONG len);   /* crc starts at 0 */

/* --- out.c: serial (section 6) --- */
void ser_init(int pal, ULONG baud);
void ser_putc(int c);
void ser_write(const char *s, ULONG len);

/* --- out.c: mailbox (section 7) --- */
#define MBOX_STATE_INIT     0
#define MBOX_STATE_RUNNING  1
#define MBOX_STATE_DONE     2
#define MBOX_STATE_ABORTED  3
#define MBOX_STATE_HALTED   4
#define MBOX_RING_SIZE      32768

struct amt_mbox {
    char  magic[16];         /* "AMITORTURE-MBOX!" */
    ULONG version;
    ULONG state;
    ULONG heartbeat;
    ULONG written;
    ULONG ring_size;
    ULONG cur_mod;
    ULONG cur_case;
    ULONG host_ack;          /* written by the HOST: id of the last request done */
    UBYTE ring[MBOX_RING_SIZE];
};
extern volatile struct amt_mbox *amt_mbox;

int  mbox_init(void);                 /* 0 on success */
void mbox_close(void);
void mbox_set_state(ULONG state);
void mbox_set_pos(ULONG mod, ULONG cas);
void mbox_beat(void);

/* --- out.c: JSON record writer (section 5) ---
 * json_begin(type) starts {"v":1,"seq":N,"t":"type"; the field helpers take a
 * key (NULL inside arrays); json_end() appends " *crc" and sends the line to
 * serial and the mailbox. */
void json_begin(const char *type);
void json_str(const char *key, const char *val);
void json_hex(const char *key, ULONG val);      /* 8 lowercase hex digits */
void json_hexn(const char *key, ULONG val, int digits);
void json_int(const char *key, long val);
void json_bool(const char *key, int val);       /* emitted as 0/1 */
void json_raw(const char *key, const char *json); /* verbatim JSON value */
void json_obj(const char *key);                 /* opens { */
void json_obj_end(void);
void json_arr(const char *key);                 /* opens [ */
void json_arr_end(void);
void json_end(void);
ULONG json_records(void);                        /* records sent so far */

void amt_log(const char *msg);

/* small string helpers (no libc) */
ULONG amt_strlen(const char *s);
char *amt_utoa(char *dst, ULONG v);              /* returns end, NUL-terminated */
char *amt_vstr(char *dst, UWORD ver, UWORD rev); /* "ver.rev" */

/* --- takeover.c / takeover.s --- */
struct amt_saved {
    UWORD intena, dmacon, adkcon, pad;
    ULONG vbr;
    ULONG vec[64];           /* vec[2..63] used */
};
void takeover_save(struct amt_saved *s);        /* call after Disable() */
void takeover_take(void);                        /* all interrupts and DMA off */
void takeover_restore(const struct amt_saved *s);
ULONG amt_get_vbr(void);                         /* 68010+ only (takeover_010.s) */
void amt_coldreboot_ks1(void);                   /* takeover.s; never returns */

#endif
