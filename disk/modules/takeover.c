/* takeover: the runtime can take the machine over and give it back intact.
 * Runs with exec alive (AMT_PH_OS) and does its own Disable()/restore, so
 * it checks the same save/restore code the bare phase uses. */
#include <proto/exec.h>
#include "mod.h"

AMT_MODULE(takeover, 0, AMT_PH_OS, 0, 0, 0);

void amt_run_takeover(void)
{
    static struct amt_saved s;
    UWORD intena, dmacon;
    ULONG bad_vec = 0;
    volatile ULONG *v;
    int i;

    Forbid();
    Disable();
    takeover_save(&s);
    takeover_take();
    takeover_restore(&s);
    intena = CREG(R_INTENAR);
    dmacon = CREG(R_DMACONR);
    v = (volatile ULONG *)s.vbr;
    for (i = 2; i < 64; i++)
        if (v[i] != s.vec[i])
            bad_vec++;
    Enable();
    Permit();

    if ((intena & 0x7fff) == (s.intena & 0x7fff) &&
        (dmacon & 0x07ff) == (s.dmacon & 0x07ff) && bad_vec == 0) {
        amt_case("takeover_restore", AMT_ST_PASS, 0);
        return;
    }
    amt_case_open("takeover_restore", AMT_ST_FAIL);
    json_obj("expect");
    json_hexn("intena", s.intena & 0x7fff, 4);
    json_hexn("dmacon", s.dmacon & 0x07ff, 4);
    json_int("bad_vectors", 0);
    json_obj_end();
    json_obj("got");
    json_hexn("intena", intena & 0x7fff, 4);
    json_hexn("dmacon", dmacon & 0x07ff, 4);
    json_int("bad_vectors", (long)bad_vec);
    json_obj_end();
    json_str("why", "hardware state differs after takeover_restore");
    json_obj("ctx");
    json_hex("vbr", s.vbr);
    json_obj_end();
    amt_case_close();
}
