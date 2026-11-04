#include <proto/exec.h>
/* misc_hw: motherboard and custom-machine hardware other than the custom
 * chips (CONTRACT.md section 10, module id 19, bare phase):
 *   - battery-backed clock at $DC0000: OKI MSM6242B or Ricoh RP5C01/RF5C01A
 *     (misc_hw_rtc.c)
 *   - Gayle (A600/A1200): ID register, interrupt/status/enable/config
 *     registers, IDE task file, PCMCIA attribute (CIS) and common memory
 *     (misc_hw_gayle.c)
 *   - Akiko (CD32): ID, chunky-to-planar converter, NVRAM I2C bus
 *     (misc_hw_cd.c)
 *   - CDTV: DMAC registers and the bookmark RAM (misc_hw_cd.c)
 * Zorro autoconfig consistency is the OS-phase module misc_hw_os (id 24).
 *
 * Each part runs only when the hardware is detected, and detection never
 * writes to an address that could be unmapped (see the parts). Each part
 * runs inside its own amt_try() so a bus error in one does not lose the
 * others, and restores what it changed. */
#include "misc_hw.h"

AMT_MODULE(misc_hw, 19, AMT_PH_BARE, AMT_MF_LONG, 0, 0);

/* ------------------------------------------------------------ helpers */
ULONG mh_cck_ms(ULONG ms)
{
    return (ULONG)(((unsigned long long)amt_cck_hz() * ms) / 1000ULL);
}

void mh_wait_cck(ULONG cck)
{
    ULONG t0 = amt_ticks();
    while (amt_ticks() - t0 < cck)
        ;
}

void mh_wait_ms(ULONG ms)
{
    ULONG t0 = amt_ticks(), n = mh_cck_ms(ms);
    while (amt_ticks() - t0 < n)
        ;
    amt_watchdog_kick();
}

ULONG mh_ms_since(ULONG t0)
{
    return (ULONG)(((unsigned long long)(amt_ticks() - t0) * 1000ULL) / amt_cck_hz());
}

static char idb[64];
const char *mh_id(const char *a, const char *b)
{
    char *d = idb;
    while (*a && d < idb + 40)
        *d++ = *a++;
    if (b) {
        *d++ = '_';
        while (*b && d < idb + sizeof(idb) - 1)
            *d++ = *b++;
    }
    *d = 0;
    return idb;
}

char *mh_hex(char *d, ULONG v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; i--)
        *d++ = hx[(v >> (i * 4)) & 15];
    *d = 0;
    return d;
}

char *mh_cpy(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

static int name_eq(const char *a, const char *b)
{
    if (!a)
        return 0;
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

void *mh_find_name(struct List *l, const char *name)
{
    struct Node *n;
    for (n = l->lh_Head; n && n->ln_Succ; n = n->ln_Succ)
        if (name_eq(n->ln_Name, name))
            return n;
    return 0;
}

int mh_ram_overlaps(ULONG lo, ULONG hi)
{
    struct Node *n;
    for (n = SysBase->MemList.lh_Head; n && n->ln_Succ; n = n->ln_Succ) {
        struct MemHeader *mh = (struct MemHeader *)n;
        ULONG a = (ULONG)mh->mh_Lower, b = (ULONG)mh->mh_Upper;
        if (a < hi && b > lo)
            return 1;
    }
    return 0;
}

int mh_is_aros(void)
{
    const char *s = (const char *)SysBase->LibNode.lib_IdString;
    int i;
    /* AROS's exec ID string does not say "AROS" ("exec 51.10 (...)"); its
     * aros.library resident does. FindResident only walks exec's
     * ResModules array, so it is safe in the bare phase too. */
    if (FindResident((CONST_STRPTR)"aros.library"))
        return 1;
    if (!s)
        return 0;
    for (i = 0; s[i] && i < 80; i++)
        if (s[i] == 'A' && s[i + 1] == 'R' && s[i + 2] == 'O' && s[i + 3] == 'S')
            return 1;
    return 0;
}

static ULONG try_addr;
static UWORD try_val;
static void try_rd16_fn(void) { try_val = hw_rd16(try_addr); }
int mh_try_rd16(ULONG a, UWORD *v)
{
    try_addr = a;
    if (amt_try(try_rd16_fn))
        return 0;
    *v = try_val;
    return 1;
}

/* ------------------------------------------------------------ body */
static void part(const char *name, void (*fn)(void))
{
    int code = amt_try(fn);
    if (!code)
        return;
    amt_case_open(mh_id(name, "aborted"), AMT_ST_FAIL);
    json_str("why", code == AMT_ABORT_UNEXPECTED
             ? "unexpected exception inside this part (bus error = an access to hardware that did not answer)"
             : "the part was aborted");
    json_obj("expect");
    json_str("exception", "none");
    json_obj_end();
    json_obj("got");
    json_int("code", code);
    json_int("vector", amt_last_exc.vector);
    json_hexn("sr", amt_last_exc.sr, 4);
    json_hex("pc", amt_last_exc.pc);
    json_obj_end();
    amt_case_close();
    /* the hardware state of the part is unknown now: keep interrupts quiet */
    amt_set_sr(0x2000);
}

void amt_run_misc_hw(void)
{
    part("rtc", mh_rtc);
    part("gayle", mh_gayle);
    part("akiko", mh_akiko);
    part("cdtv", mh_cdtv);
    amt_case("ciaa_ovl_rom_overlay", AMT_ST_SKIP,
             "never tested: setting CIA-A PRA OVL would map the ROM over chip RAM at $000000 "
             "under the running test (vectors, stack, pools)");
}
