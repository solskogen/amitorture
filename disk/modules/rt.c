/* rt: self-test of the bare-phase framework (CONTRACT.md section 10).
 * If these fail, no other bare module's results can be trusted. */
#include "mod.h"

AMT_MODULE(rt, 1, AMT_PH_BARE, 0, 0, 0);

/* rt_asm.s */
void rt_illegal(void);
void rt_trap0(void);
void rt_trap0_handler(void);
extern volatile ULONG rt_trap_count;

static ULONG hot_fn(ULONG i)
{
    ULONG a = 0x12345678UL, b = 0x9abcdef0UL, k;
    (void)i;
    for (k = 0; k < 8; k++) {
        a = (a << 3) ^ (a >> 29) ^ b;
        b += a * 7;
    }
    return a ^ b;
}

void amt_run_rt(void)
{
    ULONG f0, f1, t0, t1, first, bad;
    int code;
    long r;
    amt_vec_fn old;

    /* 1. the VBL clock runs while we are in the bare phase */
    f0 = amt_frames_now();
    t0 = amt_ticks();
    while (amt_frames_now() - f0 < 3)
        ;
    f1 = amt_frames_now();
    t1 = amt_ticks();
    if (f1 - f0 >= 3 && t1 > t0)
        amt_case("vbl_clock", AMT_ST_PASS, 0);
    else {
        amt_case_open("vbl_clock", AMT_ST_FAIL);
        json_obj("got");
        json_int("frames", (long)(f1 - f0));
        json_hex("ticks0", t0);
        json_hex("ticks1", t1);
        json_obj_end();
        json_obj("expect");
        json_str("frames", ">=3");
        json_str("ticks", "increasing");
        json_obj_end();
        json_str("why", "vertical-blank interrupt (level 3, VERTB) not counting in the bare phase");
        amt_case_close();
    }

    /* 2. an ILLEGAL instruction inside a nested amt_try lands in the
     *    catch-all and unwinds to the inner try only */
    code = amt_try(rt_illegal);
    if (code == AMT_ABORT_UNEXPECTED && amt_last_exc.vector == 4)
        amt_case("catchall_illegal", AMT_ST_PASS, 0);
    else {
        amt_case_open("catchall_illegal", AMT_ST_FAIL);
        json_obj("expect");
        json_int("code", AMT_ABORT_UNEXPECTED);
        json_int("vector", 4);
        json_obj_end();
        json_obj("got");
        json_int("code", code);
        json_int("vector", amt_last_exc.vector);
        json_hex("pc", amt_last_exc.pc);
        json_obj_end();
        json_str("why", "ILLEGAL ($4AFC) did not take vector 4 to the runtime catch-all");
        amt_case_close();
    }
    if ((amt_get_sr() & 0x0700) == 0x0000)
        amt_case("try_restores_sr", AMT_ST_PASS, 0);
    else
        amt_check32("try_restores_sr", "sr", 0x2000, amt_get_sr(),
                    "SR not restored by amt_try after an abort");

    /* 3. a module's own vector: TRAP #0 (vector 32) */
    rt_trap_count = 0;
    old = amt_vec_set(32, rt_trap0_handler);
    rt_trap0();
    rt_trap0();
    amt_vec_set(32, old);
    amt_check32("own_vector_trap0", "count", 2, rt_trap_count,
                "TRAP #0 handler installed with amt_vec_set did not run exactly twice");

    /* 4. hot-loop repetition gives the same answer every time */
    r = amt_hot(hot_fn, amt_hot_reps(), &first, &bad);
    if (r < 0)
        amt_case("hot_repeat", AMT_ST_PASS, 0);
    else {
        amt_case_open("hot_repeat", AMT_ST_FAIL);
        json_obj("expect");
        json_hex("result", first);
        json_obj_end();
        json_obj("got");
        json_hex("result", bad);
        json_int("iteration", r);
        json_obj_end();
        json_str("why", "pure integer loop gave a different result on a later iteration");
        amt_case_close();
    }
}
