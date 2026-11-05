/* amitorture v1 main flow (milestone 1) */
#include <proto/exec.h>
#include <exec/memory.h>
#include "rt.h"
#include "mod.h"
#include "screen.h"

struct ExecBase *SysBase;
const struct amt_header *amt_hdr;
struct IOStdReq *amt_bootio;
void *amt_base;
struct amt_params amt_par;

#define probe amt_probe_result
static char probe_json[6144];

int amt_run_modules(void);   /* mod.c */
extern ULONG amt_n_pass, amt_n_fail, amt_n_skip, amt_n_untestable, amt_n_aborted;

static void load_params(void)
{
    const struct amt_params *src = &amt_hdr->params;
    amt_par.mode = AMT_MODE_FULL;
    amt_par.seed = 1;
    amt_par.end_action = AMT_END_HALT;
    amt_par.flags = 0;
    amt_par.baud = 115200;
    amt_par.modmask[0] = amt_par.modmask[1] = 0xffffffffUL;
    if (src->magic[0] == 'P' && src->magic[1] == 'A' && src->magic[2] == 'R' && src->magic[3] == 'M') {
        amt_par = *src;
        if (amt_par.baud == 0)
            amt_par.baud = 115200;
    }
}

static void emit_hello(void)
{
    char build[65];
    char vs[16];
    int i;

    for (i = 0; i < 64 && amt_hdr->build_id[i]; i++)
        build[i] = amt_hdr->build_id[i];
    build[i] = 0;

    json_begin("hello");
    json_str("build", build);
    json_int("mode", (long)amt_par.mode);
    json_int("seed", (long)amt_par.seed);
    json_obj("policy");
    json_str("integer", (amt_par.flags & AMT_F_INTCOMPAT) ? "compat" : "strict");
    json_str("fpu", (amt_par.flags & AMT_F_FPUCOMPAT) ? "compat" : "strict");
    json_obj_end();
    amt_vstr(vs, SysBase->LibNode.lib_Version, SysBase->LibNode.lib_Revision);
    json_str("exec", vs);
    json_str("cpu", probe.cpu);
    json_str("fpu", probe.fpu);
    json_int("mmu", probe.mmu);
    amt_json_modules();

    /* single serialiser for the probe object: disk/probe/probe.c */
    if (amt_probe_json(&probe, probe_json, sizeof probe_json) >= 0)
        json_raw("probe", probe_json);
    else
        json_str("probe_error", "probe JSON exceeds buffer");
    json_end();
}

int amt_main(void)
{
    load_params();
    mbox_init();
    ser_init(SysBase->VBlankFrequency != 60, amt_par.baud);

    amt_probe_with_claims(&probe, amt_par.end_action == AMT_END_HALT ||
                                  amt_par.end_action == AMT_END_REBOOT);
    ser_init(probe.pal, amt_par.baud);   /* now with the probed clock */

    emit_hello();
    mbox_set_state(MBOX_STATE_RUNNING);

    if (amt_run_modules() < 0)
        amt_n_aborted++;

    json_begin("done");
    json_int("pass", (long)amt_n_pass);
    json_int("fail", (long)amt_n_fail);
    json_int("skip", (long)amt_n_skip);
    json_int("untestable", (long)amt_n_untestable);
    json_int("aborted", (long)amt_n_aborted);
    json_int("records", (long)json_records() + 1);   /* including this one */
    json_end();
    mbox_set_state(MBOX_STATE_DONE);
    screen_finish(amt_n_fail, amt_n_aborted);

    switch (amt_par.end_action) {
    case AMT_END_REBOOT:
        mbox_set_state(MBOX_STATE_HALTED);
        if (SysBase->LibNode.lib_Version >= 36)
            ColdReboot();
        amt_coldreboot_ks1();
        /* not reached */
        break;
    case AMT_END_RETURN:
        screen_close();
        amt_probe_release_boards(&probe);
        amt_return_cleanup();
        mbox_close();
        return -1;
    default:
        break;
    }
    mbox_set_state(MBOX_STATE_HALTED);
    for (;;)
        Wait(0);
    return 0;
}
