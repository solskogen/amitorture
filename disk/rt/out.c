/* amitorture output: Paula serial (section 6), memory mailbox (section 7) and
 * the JSON line writer (section 5). */
#include <proto/exec.h>
#include <exec/memory.h>
#include "rt.h"

/* ------------------------------------------------------------------------ */
/* small string helpers */

ULONG amt_strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (ULONG)(p - s);
}

char *amt_utoa(char *dst, ULONG v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *dst++ = tmp[--n];
    *dst = 0;
    return dst;
}

char *amt_vstr(char *dst, UWORD ver, UWORD rev)
{
    dst = amt_utoa(dst, ver);
    *dst++ = '.';
    return amt_utoa(dst, rev);
}

/* ------------------------------------------------------------------------ */
/* serial */

static int ser_on;

void ser_init(int pal, ULONG baud)
{
    ULONG clock = pal ? 3546895UL : 3579545UL;
    ULONG per;
    if (amt_par.flags & AMT_F_NOSERIAL) {
        ser_on = 0;
        return;
    }
    if (baud == 0)
        baud = 115200;
    /* SERPER = clock / baud - 1, rounded: at 115200 a PAL machine gets 30
     * (114416 baud, -0.7%) instead of a truncated 29 (+2.6%, near the edge
     * of what a PC UART accepts) */
    per = (clock + baud / 2) / baud;
    if (per)
        per--;
    if (per > 0x7fff)
        per = 0x7fff;
    CREG(R_SERPER) = (UWORD)per;   /* bit 15 = 0: 8 data bits */
    ser_on = 1;
}

void ser_putc(int c)
{
    /* Wait for the transmit buffer to be empty. SERDATR's TBE (bit 13) is the
     * buffer state itself, so it works whatever INTREQ's TBE latch says.
     * The timeout keeps a dead UART from hanging the run. */
    ULONG n = 200000;
    if (!ser_on)
        return;
    while (!(CREG(R_SERDATR) & 0x2000) && --n)
        ;
    CREG(R_SERDAT) = (UWORD)(0x100 | (c & 0xff));
}

void ser_write(const char *s, ULONG len)
{
    while (len--)
        ser_putc(*s++);
}

/* ------------------------------------------------------------------------ */
/* mailbox */

volatile struct amt_mbox *amt_mbox;
static UBYTE *mbox_alloc;

int mbox_init(void)
{
    static const char magic[] = "AMITORTURE-MBOX!";
    int i;
    if (amt_mbox || (amt_par.flags & AMT_F_NOMBOX))
        return 0;
    mbox_alloc = AllocMem(sizeof(struct amt_mbox) + 16, MEMF_CHIP | MEMF_CLEAR);
    if (!mbox_alloc)
        return -1;
    amt_mbox = (volatile struct amt_mbox *)(((ULONG)mbox_alloc + 15) & ~15UL);
    amt_mbox->version = 1;
    amt_mbox->state = MBOX_STATE_INIT;
    amt_mbox->ring_size = MBOX_RING_SIZE;
    /* magic last, so a host never sees a half-built mailbox */
    for (i = 0; i < 16; i++)
        amt_mbox->magic[i] = magic[i];
    return 0;
}

void mbox_close(void)
{
    if (amt_mbox)
        amt_mbox->magic[0] = 0;
    amt_mbox = 0;
    if (mbox_alloc)
        FreeMem(mbox_alloc, sizeof(struct amt_mbox) + 16);
    mbox_alloc = 0;
}

void mbox_set_state(ULONG state)
{
    if (amt_mbox) {
        amt_mbox->state = state;
        amt_mbox->heartbeat++;
    }
}

void mbox_set_pos(ULONG mod, ULONG cas)
{
    if (amt_mbox) {
        amt_mbox->cur_mod = mod;
        amt_mbox->cur_case = cas;
    }
}

void mbox_beat(void)
{
    if (amt_mbox)
        amt_mbox->heartbeat++;
}

static void mbox_write(const char *s, ULONG len)
{
    volatile struct amt_mbox *m = amt_mbox;
    ULONG pos;
    if (!m)
        return;
    pos = m->written % MBOX_RING_SIZE;
    while (len--) {
        m->ring[pos] = (UBYTE)*s++;
        if (++pos == MBOX_RING_SIZE)
            pos = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* JSON line writer */

#define LINE_MAX 8192
#define DEPTH_MAX 8

static char line[LINE_MAX + 16];
static ULONG llen;
static int overflow;
static ULONG seq;
static UBYTE first[DEPTH_MAX];   /* 1 = next element needs no comma */
static char closer[DEPTH_MAX];
static int depth;

static void put(char c)
{
    if (llen < LINE_MAX)
        line[llen++] = c;
    else
        overflow = 1;
}

static void puts_raw(const char *s)
{
    while (*s)
        put(*s++);
}

static void put_qstr(const char *s)
{
    static const char hexd[] = "0123456789abcdef";
    put('"');
    for (; *s; s++) {
        UBYTE c = (UBYTE)*s;
        if (c == '"' || c == '\\') {
            put('\\');
            put((char)c);
        } else if (c < 0x20 || c > 0x7e) {
            puts_raw("\\u00");
            put(hexd[c >> 4]);
            put(hexd[c & 15]);
        } else {
            put((char)c);
        }
    }
    put('"');
}

static void put_key(const char *key)
{
    if (first[depth])
        first[depth] = 0;
    else
        put(',');
    if (key) {
        put_qstr(key);
        put(':');
    }
}

static void put_hex(ULONG v, int digits)
{
    static const char hexd[] = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; i--)
        put(hexd[(v >> (i * 4)) & 15]);
}

void json_begin(const char *type)
{
    char num[12];
    llen = 0;
    overflow = 0;
    depth = 0;
    first[0] = 1;
    put('{');
    put_key("v");
    put('1');
    put_key("seq");
    amt_utoa(num, seq);
    puts_raw(num);
    put_key("t");
    put_qstr(type);
}

void json_str(const char *key, const char *val)
{
    put_key(key);
    put_qstr(val ? val : "");
}

/* Insert pre-serialised JSON (an object/array/value produced elsewhere,
 * e.g. amt_probe_json()) verbatim under key. The caller guarantees it is
 * valid, ASCII-only JSON without newlines. */
void json_raw(const char *key, const char *json)
{
    put_key(key);
    puts_raw(json ? json : "null");
}

void json_hexn(const char *key, ULONG val, int digits)
{
    put_key(key);
    put('"');
    put_hex(val, digits);
    put('"');
}

void json_hex(const char *key, ULONG val)
{
    json_hexn(key, val, 8);
}

void json_int(const char *key, long val)
{
    char num[12];
    put_key(key);
    if (val < 0) {
        put('-');
        amt_utoa(num, (ULONG)-val);
    } else {
        amt_utoa(num, (ULONG)val);
    }
    puts_raw(num);
}

void json_bool(const char *key, int val)
{
    put_key(key);
    put(val ? '1' : '0');
}

static void open_nest(const char *key, char c)
{
    put_key(key);
    put(c);
    if (depth < DEPTH_MAX - 1)
        depth++;
    else
        overflow = 1;
    first[depth] = 1;
    closer[depth] = c == '{' ? '}' : ']';
}

static void close_nest(void)
{
    if (depth > 0) {
        put(closer[depth]);
        depth--;
    }
}

void json_obj(const char *key) { open_nest(key, '{'); }
void json_obj_end(void) { close_nest(); }
void json_arr(const char *key) { open_nest(key, '['); }
void json_arr_end(void) { close_nest(); }

static void send_line(void)
{
    ULONG crc;
    while (depth > 0)        /* tolerate a missing close */
        close_nest();
    put('}');
    crc = crc32_update(0, line, llen);
    /* LINE_MAX leaves room for the suffix */
    line[llen++] = ' ';
    line[llen++] = '*';
    {
        static const char hexd[] = "0123456789abcdef";
        int i;
        for (i = 7; i >= 0; i--)
            line[llen++] = hexd[(crc >> (i * 4)) & 15];
    }
    line[llen++] = '\n';
    ser_write(line, llen);
    mbox_write(line, llen);
    if (amt_mbox)
        amt_mbox->written += llen;
    seq++;
    mbox_beat();
}

void json_end(void)
{
    if (overflow) {
        /* the record did not fit: replace it with a log record */
        char num[12];
        json_begin("log");
        put_key("msg");
        puts_raw("\"record too long, dropped (seq ");
        amt_utoa(num, seq);
        puts_raw(num);
        puts_raw(")\"");
    }
    send_line();
}

ULONG json_records(void)
{
    return seq;
}

void amt_log(const char *msg)
{
    json_begin("log");
    json_str("msg", msg);
    json_end();
}
