/* exceptions (module id 4): every exception the CPU can take from software,
 * per detected variant. For each one it checks the vector taken, the size of
 * the frame, the stacked SR and PC, the format/vector-offset word (68010+),
 * the format $2 instruction address (68020+), the SR the handler starts
 * with, and the condition codes of the instruction that trapped.
 *
 * The recording handlers and the snippets live in exceptions_asm.s; see the
 * comment there. Expectations come from the M68000 PRM (section 1.6/B,
 * instruction pages), the MC68000/68010 UM (exception processing), the
 * MC68020/030 UM (section 6 / 8: exception stack frames), the MC68040 UM
 * (section 8) and the MC68060 UM (section 8). Docs: docs/cases/exceptions.md.
 *
 * Records are byte-identical for the same variant: PCs are reported as
 * offsets from the instruction under test, absolute addresses only appear
 * in the ctx of a failing case. */
#include "mod.h"

AMT_MODULE(exceptions, 4, AMT_PH_BARE, 0, 0, 4096);

/* ------------------------------------------------------------ asm side */
#define XLOGMAX 40
struct xlog { UWORD vec, hsr; ULONG ssp; UWORD f[12]; };

extern volatile UWORD xr_vec, xr_hsr, xr_mode, xr_nlog, xr_skipvec, xr_srin;
extern volatile ULONG xr_ssp, xr_spbase, xr_stop_pc, xr_usp, xr_ina0, xr_ina1;
extern volatile UWORD xr_frame[30];
extern volatile ULONG xr_regs[15], xr_in[8], xr_out[8], xr_old[64];
extern const ULONG xr_stub_table[62];
extern volatile struct xlog xr_log[XLOGMAX];
ULONG xr_call(void (*fn)(void));
ULONG xr_get_usp(void);
void xr_set_usp(ULONG usp);

extern volatile UWORD xn_hsr1, xn_hsr2;
extern volatile ULONG xn_ssp1, xn_ssp1b, xn_ssp2;
extern volatile UWORD xn_f1[8], xn_f1b[8], xn_f2[8];
void xh_nest1(void);
void xh_nest2(void);
extern UBYTE xh_nest1_at[], xh_nest1_nx[];

#define XS(n) extern UBYTE xs_##n[], xs_##n##_at[], xs_##n##_nx[]
extern UBYTE xs_trap[], xs_tcc[], xs_ill[], xs_ill_end[], xs_aetgt[], xs_mis[];
XS(trapv); XS(chkw); XS(chkwi); XS(chkl);
XS(chk2l); XS(cmp2l); XS(chk2b); XS(cmp2w); XS(chk2wa);
XS(divuw); XS(divsw); XS(divuwi); XS(divswi); XS(divul); XS(divsl);
XS(divull); XS(divsll); XS(divul64); XS(divsl64); XS(divuli);
XS(mulul64); XS(mulsl64); XS(movepw); XS(movepl); XS(cas2w); XS(cas2l); XS(casw);
XS(fnop);
XS(umovesr); XS(umovefsr); XS(uandisr); XS(uorisr); XS(ueorisr); XS(umoveusp);
XS(umovec); XS(umoves); XS(urte); XS(utrap); XS(uaerr);
XS(aerw); XS(aerl); XS(aeww); XS(aewl); XS(aejmp); XS(berr);
XS(nest); XS(rte); XS(rte1);
extern UBYTE xs_rte1_bad[];
extern UBYTE xs_tr1[], xs_tr1_a[], xs_tr1_b[], xs_tr1_c[], xs_tr1_d[], xs_tr1_e[],
    xs_tr1_f[], xs_tr1_g[], xs_tr1_h[], xs_tr1_sub[], xs_tr1_s1[];
extern UBYTE xs_tr0[], xs_tr0_a[], xs_tr0_b[], xs_tr0_c[], xs_tr0_d[], xs_tr0_e[],
    xs_tr0_f[], xs_tr0_g[], xs_tr0_x[], xs_tr0_end[], xs_tr0_sub[], xs_tr0_s1[];
extern UBYTE xs_trt[], xs_trt_at[], xs_trt_nx[], xs_trt_n2[];
extern UBYTE xs_tri[], xs_tri_at[], xs_tri_nx[], xs_tri_n2[];

#define A(x) ((ULONG)(x))

/* ------------------------------------------------------------ variant */
static LONG cpu;
static int fam;                  /* 0 000, 1 010, 2 020, 3 030, 4 040, 5 060 */
#define F000 0x01
#define F010 0x02
#define F020 0x04
#define F030 0x08
#define F040 0x10
#define F060 0x20
#define FALL 0x3f
#define FBIT (1 << fam)
#define IS000 (fam == 0)
#define IS010 (fam == 1)
#define GE020 (fam >= 2)
#define IS060 (fam == 5)

static UBYTE *pool;
#define P_USTK   1024            /* user stack top */
#define P_MIS    1024            /* misaligned-access area (32 bytes) */
#define P_BND    1088            /* CHK2/CMP2 bounds */
#define P_CAS    1120            /* CAS/CAS2/MOVEP operands (16 bytes) */
#define P_AE     1200            /* address-error targets */

static const char *const fam_name[6] = { "68000", "68010", "68020", "68030", "68040", "68060" };

/* ------------------------------------------------------------ checking */
#define T_H4  0
#define T_H8  1
#define T_INT 2
struct fld { const char *k; ULONG e, g; UBYTE t; };
#define MAXF 14
#define MAXI 8
struct run {
    int nf;
    struct fld f[MAXF];
    int ni;
    const char *ik[MAXI];
    ULONG iv[MAXI];
    ULONG at;
    int nfr;
    UWORD fr[16];
    int ev;
    int grp0;                   /* fr[] is a 68000 group 0 frame */
    const char *pcrule;         /* stacked-PC rule applied, or 0 */
    int nev;                    /* trace log copy: vec, sr, pc_off, addr_off */
    long evd[12][4];
};
static struct run R, F1;
static int k_tot, k_fail;
static const char *k_why;

static void kbegin(const char *why)
{
    k_tot = k_fail = 0;
    k_why = why;
}

static void rbegin(ULONG at)
{
    R.nf = 0;
    R.ni = 0;
    R.at = at;
    R.nfr = 0;
    R.ev = -1;
    R.grp0 = 0;
    R.pcrule = 0;
    R.nev = 0;
    k_tot++;
}

static void rin(const char *k, ULONG v)
{
    if (R.ni < MAXI) {
        R.ik[R.ni] = k;
        R.iv[R.ni++] = v;
    }
}

static int rcmp(const char *k, ULONG e, ULONG g, int t)
{
    if (e == g)
        return 1;
    if (R.nf < MAXF) {
        R.f[R.nf].k = k;
        R.f[R.nf].e = e;
        R.f[R.nf].g = g;
        R.f[R.nf].t = (UBYTE)t;
        R.nf++;
    }
    return 0;
}

/* compare two addresses, reported as offsets from the instruction under test */
static int rcmpo(const char *k, ULONG e, ULONG g)
{
    return rcmp(k, e - R.at, g - R.at, T_INT);
}

static void rframe_from(volatile UWORD *f, int n)
{
    int i;
    R.nfr = n > 16 ? 16 : n;
    for (i = 0; i < R.nfr; i++)
        R.fr[i] = f[i];
}

static void rend(void)
{
    if (R.nf) {
        k_fail++;
        if (k_fail == 1)
            F1 = R;
    }
}

static void kput(const struct fld *f, int got)
{
    ULONG v = got ? f->g : f->e;
    if (f->t == T_H4)
        json_hexn(f->k, v, 4);
    else if (f->t == T_H8)
        json_hex(f->k, v);
    else
        json_int(f->k, (long)v);
}

static const char FIELDS[] =
    "*_off fields (pc_off, addr_off, fault_pc_off, ev_pc_off, ev_addr_off, extra_pc_off, f1/f2_pc_off) "
    "= the address held in the stack frame minus insn_addr (the address of the instruction under "
    "test), signed decimal; addr_off is the format $2 'instruction address' field, fault_pc_off the "
    "format $4 'PC of faulted instruction' field. vec = vector number taken (0 = no exception, "
    "65535 = runaway); frame_size = bytes pushed by the exception; sr = stacked SR (only the bits "
    "the rule defines are compared); fv = format/vector-offset word (format<<12 | vector*4); "
    "handler_sr = SR at handler entry (must equal the stacked SR with S=1 and T1/T0=0); sr_after = "
    "SR after the instruction when no exception is expected; ctx.frame = raw stacked words from "
    "SP upward";

static const char *frame_layout(const struct run *r)
{
    if (!r->nfr)
        return 0;
    if (r->grp0)
        return "68000 group 0 (14 bytes): status.w (bit4 R/W 1=read, bit3 I/N 0=instruction, "
               "bits2-0 function code) access_addr.l IR.w SR.w PC.l";
    if (IS000)
        return "68000 group 1/2 (6 bytes, no format word): SR.w PC.l";
    switch (r->fr[3] >> 12) {
    case 0: return "format $0 (8 bytes): SR.w PC.l fmt/vec.w";
    case 1: return "format $1 throwaway (8 bytes): SR.w PC.l fmt/vec.w";
    case 2: return "format $2 (12 bytes): SR.w PC.l fmt/vec.w addr.l (addr = address of the "
                   "instruction that caused the exception; for trace the traced instruction)";
    case 3: return "format $3 (12 bytes): SR.w PC.l fmt/vec.w effective_address.l";
    case 4: return "format $4 (16 bytes): SR.w PC.l fmt/vec.w ea_or_fault_addr.l "
                   "faulted_pc_or_fslw.l";
    case 7: return "format $7 68040 access error (60 bytes): SR.w PC.l fmt/vec.w ea.l ssw.w ...";
    case 8: return "format $8 68010 bus/address error (58 bytes): SR.w PC.l fmt/vec.w SSW.w "
                   "fault_addr.l unused.w DOB.w unused.w DIB.w unused.w IIB.w internal[16].w";
    case 10: return "format $A 68020/030 short bus error (32 bytes): SR.w PC.l fmt/vec.w "
                    "internal.w SSW.w pipe_c.w pipe_b.w fault_addr.l ...";
    case 11: return "format $B 68020/030 long bus error (92 bytes): SR.w PC.l fmt/vec.w "
                    "internal.w SSW.w pipe_c.w pipe_b.w fault_addr.l ...";
    }
    return "unknown format";
}

static void kend(const char *id)
{
    int i;
    const UWORD *op;
    if (!k_fail) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    for (i = 0; i < F1.nf; i++)
        kput(&F1.f[i], 0);
    json_obj_end();
    json_obj("got");
    for (i = 0; i < F1.nf; i++)
        kput(&F1.f[i], 1);
    json_obj_end();
    json_str("why", k_why);
    json_obj("in");
    for (i = 0; i < F1.ni; i++)
        json_hex(F1.ik[i], F1.iv[i]);
    json_obj_end();
    json_obj("ctx");
    json_str("cpu", amt_probe_result.cpu);
    json_hex("insn_addr", F1.at);
    json_str("fields", FIELDS);
    if (frame_layout(&F1))
        json_str("frame_layout", frame_layout(&F1));
    if (F1.pcrule)
        json_str("pc_rule", F1.pcrule);
    op = (const UWORD *)F1.at;
    json_arr("op");
    for (i = 0; i < 3; i++)
        json_hexn(NULL, op[i], 4);
    json_arr_end();
    if (F1.nfr) {
        json_arr("frame");
        for (i = 0; i < F1.nfr; i++)
            json_hexn(NULL, F1.fr[i], 4);
        json_arr_end();
    }
    if (F1.ev >= 0)
        json_int("event", F1.ev);
    if (F1.nev) {
        json_arr("events");
        for (i = 0; i < F1.nev; i++) {
            json_arr(NULL);
            json_int(NULL, F1.evd[i][0]);
            json_hexn(NULL, (ULONG)F1.evd[i][1], 4);
            json_int(NULL, F1.evd[i][2]);
            json_int(NULL, F1.evd[i][3]);
            json_arr_end();
        }
        json_arr_end();
        json_str("events_layout", "[vector, stacked SR, PC offset, format-2 address offset (020+)]");
    }
    json_int("runs", k_tot);
    json_int("failed_runs", k_fail);
    json_obj_end();
    amt_case_close();
}

static void skip(const char *id, const char *why)
{
    amt_case(id, AMT_ST_SKIP, why);
}

/* ------------------------------------------------------------ running */
static void xset(UWORD sr)
{
    int i;
    for (i = 0; i < 8; i++)
        xr_in[i] = 0;
    xr_ina0 = xr_ina1 = 0;
    xr_srin = sr;
    xr_mode = 0;
    xr_skipvec = 0;
    xr_stop_pc = 0;
}

static ULONG xrun(ULONG fn)
{
    return xr_call((void (*)(void))fn);
}

static ULONG frl(volatile UWORD *f, int w)
{
    return ((ULONG)f[w] << 16) | f[w + 1];
}

/* stack frame format for an exception taken from an instruction (not
 * group 0): 68010 always $0; 68020+: $2 for CHK/CHK2 (6), zero divide (5),
 * TRAPcc/TRAPV (7) and trace (9), $0 otherwise */
static int deffmt(int vec)
{
    if (GE020 && (vec == 5 || vec == 6 || vec == 7 || vec == 9))
        return 2;
    return 0;
}

static ULONG fsize(int fmt)
{
    switch (fmt) {
    case 0: case 1: return 8;
    case 2: case 3: return 12;
    case 4: return 16;
    case 7: return 60;
    case 8: return 58;
    case 9: return 20;
    case 10: return 32;
    case 11: return 92;
    }
    return 0;
}

#define X_NOSIZE 1
#define X_NOPC   2

/* Check the exception recorded by the last xr_call against: vector vec (0 =
 * no exception), stacked SR (sr & srm), stacked PC pc, the 68020+ format $2
 * instruction address addr (0 = not checked) and format fmt (-1 = default
 * for vec). */
static void rexc(UWORD vec, UWORD sr, UWORD srm, ULONG pc, ULONG addr, int fmt, int flags)
{
    UWORD gv = xr_vec;
    if (gv)
        rframe_from(xr_frame, IS000 ? 8 : 12);
    if (!rcmp("vec", vec, gv, T_INT) || !vec)
        return;
    if (fmt < 0)
        fmt = deffmt(vec);
    if (!(flags & X_NOSIZE))
        rcmp("frame_size", IS000 ? 6 : fsize(fmt), xr_spbase - xr_ssp, T_INT);
    rcmp("sr", sr & srm, xr_frame[0] & srm, T_H4);
    if (!(flags & X_NOPC)) {
        R.pcrule = pc == R.at ? "stacked PC = the address of the instruction under test "
                                "(expected pc_off 0)"
                              : "stacked PC = the address of the next instruction (expected "
                                "pc_off = length of the instruction under test, or the offset "
                                "of the label it should resume at)";
        rcmpo("pc_off", pc, frl(xr_frame, 1));
    }
    if (!IS000)
        rcmp("fv", ((ULONG)fmt << 12) | (vec * 4UL), xr_frame[3], T_H4);
    if (!IS000 && fmt == 2 && addr)
        rcmpo("addr_off", addr, frl(xr_frame, 4));
    /* exception processing: S set, T1/T0 cleared, IPL and CCR (and M)
     * unchanged */
    rcmp("handler_sr", (xr_frame[0] | 0x2000) & 0x3fff, xr_hsr, T_H4);
}

/* no exception expected; checks SR after the instruction (d7) if srm */
static void rnone(UWORD sr_after, UWORD srm)
{
    UWORD gv = xr_vec;
    if (gv)
        rframe_from(xr_frame, IS000 ? 8 : 12);
    if (!rcmp("vec", 0, gv, T_INT))
        return;
    if (srm)
        rcmp("sr_after", sr_after & srm, xr_out[7] & srm, T_H4);
}

/* ------------------------------------------------------------ why texts */
static const char W_TRAP[] =
    "TRAP #n: vector 32+n, stacked SR = SR before the TRAP, stacked PC = next instruction; "
    "68010+: format $0, vector offset (32+n)*4 (M68000PRM TRAP, 68020UM 6.1.4/Table 6-5)";
static const char W_TRAPV[] =
    "TRAPV: V set -> vector 7, stacked PC = next instruction, CCR unchanged; 68020+: format $2 "
    "with the TRAPV address; V clear -> no exception (M68000PRM TRAPV, 68020UM Table 6-5)";
static const char W_CHK[] =
    "CHK: Dn<0 -> vector 6 with N=1; Dn>bound (signed) -> vector 6 with N=0; else no trap. X not "
    "affected; Z,V,C undefined (masked), N undefined when no trap (masked). Stacked PC = next "
    "instruction; 68020+: format $2 with the CHK address (M68000PRM CHK, 68020UM Table 6-5)";
static const char W_CHK2[] =
    "CHK2/CMP2: Z = Rn equals either bound, C = Rn out of bounds, X not affected, N,V undefined "
    "(masked); data register compares only the operand size, address register compares 32 bits "
    "against sign-extended bounds; CHK2 out of bounds -> vector 6, format $2, PC = next "
    "instruction, address = CHK2 (M68000PRM CHK2/CMP2)";
static const char W_DIV0[] =
    "DIVU/DIVS by zero: vector 5, stacked PC = next instruction, X not affected, C cleared, "
    "N,Z,V undefined (masked), destination registers unchanged; 68020+: format $2 with the "
    "DIV address (M68000PRM DIVS/DIVU, 68020UM Table 6-5)";
static const char W_ILL[] =
    "unassigned opcode / ILLEGAL / line-A / line-F: illegal instruction vector 4 (line A 10, "
    "line F 11), stacked PC = address of the opcode, SR unchanged; 68010+: format $0 "
    "(M68000PRM 1.6 / Table B-1, 68000UM 6.3.6/6.3.7)";
static const char W_PRIV[] =
    "privileged instruction in user mode: vector 8, stacked PC = the privileged instruction, "
    "stacked SR = user SR (S=0), handler SR has S=1; 68010+: format $0 (M68000PRM 1.6, "
    "68000UM 6.3.8; MOVE from SR, MOVEC, MOVES privileged on 68010+)";
static const char W_UTRAP[] =
    "TRAP in user mode: the frame goes on the supervisor stack (ISP) with the user SR (S=0) and "
    "PC = next instruction (M68000PRM TRAP)";
static const char W_MFSR000[] =
    "68000: MOVE from SR is not privileged; in user mode it reads the user SR without an "
    "exception (M68000PRM MOVE from SR: privileged on 68010+ only)";
static const char W_MOVEC000[] =
    "68000: MOVEC does not exist; in user mode it is an illegal instruction (vector 4), not a "
    "privilege violation (M68000PRM MOVEC: 68010+)";
static const char W_TRAPCC[] =
    "TRAPcc (68020+): condition true -> vector 7, format $2, PC = next instruction (after the "
    "operand words), address = TRAPcc, SR unchanged; false -> no exception (M68000PRM TRAPcc, "
    "Table 3-19 condition tests)";
static const char W_AE000[] =
    "68000 address error: vector 3, 7-word group 0 frame: status word (bit4 R/W 1=read, bit3 "
    "I/N 0=instruction, bits 2-0 function code; bits 15-5 undefined), access address, IR "
    "(opcode), SR, PC (2..10 bytes past the instruction start) (68000UM 6.3.9.1, Figure 6-7)";
static const char W_AE010[] =
    "68010 address error: vector 3, format $8 (29 words): SSW (IF bit13 ifetch, DF bit12 data "
    "read, RW bit8 1=read, FC bits 2-0), fault address at +10, vector offset $00C (68010UM "
    "5.5.2, Figure 5-8)";
static const char W_AE020[] =
    "68020/030 instruction fetch from an odd address (JMP to an odd target): vector 3 with a "
    "bus fault frame, short format $A (32 bytes) or long format $B (92 bytes), vector offset "
    "$00C, stacked SR = SR before (68020UM 6.1.2 address error, 68030UM 8.1.2 and 8.2.1)";
static const char W_AE040[] =
    "68040/060 instruction fetch from an odd address (JMP to an odd target): vector 3, format "
    "$2 (12 bytes), the address field = the odd target address (or the target with bit 0 "
    "cleared: both accepted), vector offset $00C, stacked SR = SR before (68040UM 8.2.2, "
    "68060UM 8.2.2 address error)";
static const char W_MIS[] =
    "68020+: misaligned word/long data accesses do not trap and read/write the right bytes "
    "(68020UM 1.5.1 / 7.2.4 dynamic bus sizing)";
static const char W_FLINE_NOFPU[] =
    "FPU instruction on an FPU-less CPU: F-line vector 11; 68000-030 format $0 with PC = the "
    "instruction; 68LC/EC040 and 68LC/EC060 format $4: PC = next instruction, PC of the faulted "
    "instruction at +$0C (effective address at +8 not checked) (68020UM 7.4, 68040UM A.5.1, "
    "68060UM 8.4.4)";
static const char W_UNIMPL060[] =
    "68060 unimplemented integer instruction (MOVEP, CAS2, CAS misaligned, CHK2/CMP2, 64-bit "
    "MUL/DIV): vector 61, format $0, PC = the instruction, SR and registers/memory unchanged; "
    "no 68060.library on bare metal (68060UM 8.2.4). In Amiberry the trap only happens with the "
    "option cpu_no_unimplemented=true: with that option off (the default, and always with the "
    "JIT) the instruction is emulated, which is not what a real 68060 does";
static const char W_TRACE1[] =
    "T1 trace: after every executed instruction a trace exception (vector 9) with PC = next "
    "instruction and the SR of that point; the instruction that sets T is not traced, the one "
    "that clears it is; 68020+: format $2 with the traced instruction's address "
    "(M68000PRM 1.6, 68020UM 6.1.8)";
static const char W_TRACE0[] =
    "T0 trace (68020-68040): a trace exception only after instructions that change the flow "
    "(BRA, JSR, RTS here; on the 68040 also NOP), PC = the new flow target, format $2 with the traced instruction's "
    "address (68020UM 6.1.8, 68040UM 8.2.6)";
static const char W_TRTRAP[] =
    "trace + TRAP: the TRAP exception is processed first, then the pending trace exception, so "
    "the trace handler runs first with PC = first instruction of the TRAP handler and the "
    "trap-entry SR (S=1, T=0); the TRAP frame holds the traced SR and PC = next instruction "
    "(68000UM 6.3.8, 68020UM 6.1.8/6.1.11)";
static const char W_TRTRAP4[] =
    "trace + TRAP on the 68040/060: a trace pending together with a group 3 (040) / group 2-3 "
    "(060) exception such as TRAP is not reported; the TRAP frame holds the traced SR and PC = "
    "next instruction, and tracing resumes with the instruction after the TRAP (68040UM 8.3 "
    "last bullet, 68060UM 8.2.6 note)";
static const char W_TRILL[] =
    "trace + ILLEGAL: an instruction that is not executed (illegal) is not traced; vector 4 with "
    "the traced SR, then tracing resumes at the next instruction (68000UM 6.3.8)";
static const char W_NEST[] =
    "TRAP inside a TRAP handler: the second frame is pushed below the first on the same "
    "supervisor stack with the handler's SR and PC; the first frame stays intact and both RTE "
    "back in order (M68000PRM 1.6)";
static const char W_RTEOK[] =
    "RTE pops the whole frame of an accepted format and restores SR/PC: 68010+ format $0 (4 "
    "words), 68020+ $2 (6 words), 68020-040 $1 throwaway (then the frame below), 68040/060 $3 "
    "(6 words), 68060 $4 (8 words); the vector offset field is ignored (M68000PRM RTE, "
    "68020UM 6.4, 68040UM 8.4, 68060UM 8.4)";
static const char W_RTE000[] =
    "68000 RTE pops exactly SR and PC (6 bytes); there is no format word (M68000PRM RTE)";
static const char W_RTEBAD[] =
    "RTE with a format code the CPU does not accept: format error, vector 14, format $0, "
    "stacked PC = the RTE, SR = the SR when the RTE began (not loaded from the bad frame; "
    "68010: N,Z,V masked, the 68010 UM does not define them here). Accepted: 010 {0,8}, 020/030 "
    "{0,1,2,9,A,B}, 040 {0,1,2,3,7}, 060 {0,2,3,4} (M68000PRM RTE, 68020UM 6.1.12, 68040UM "
    "8.2.7/8.4, 68060UM 8.2.7: 'formats 0, 2, 3 or 4')";
static const char W_BERR[] =
    "read of an address no board decodes: bus error, vector 2, frame 68000 group 0 / 68010 $8 / "
    "68020-030 $A or $B / 68040 $7 / 68060 $4 with the fault address";

/* ------------------------------------------------------------ TRAP #n */
static const char *const trap_ids[16] = {
    "trap_0", "trap_1", "trap_2", "trap_3", "trap_4", "trap_5", "trap_6", "trap_7",
    "trap_8", "trap_9", "trap_10", "trap_11", "trap_12", "trap_13", "trap_14", "trap_15"
};

static void t_trap(void)
{
    int n;
    for (n = 0; n < 16; n++) {
        ULONG at = A(xs_trap) + 4UL * n;
        UWORD sr = 0x2700 | ((n * 7) & 0x1f);
        kbegin(W_TRAP);
        xset(sr);
        rbegin(at);
        rin("sr", sr);
        xrun(at);
        rexc(32 + n, sr, 0xffff, at + 2, 0, -1, 0);
        rend();
        kend(trap_ids[n]);
    }
}

/* ------------------------------------------------------------ TRAPV */
static void t_trapv(void)
{
    UWORD sr;
    kbegin(W_TRAPV);
    sr = 0x2702;
    xset(sr);
    rbegin(A(xs_trapv_at));
    rin("sr", sr);
    xrun(A(xs_trapv));
    rexc(7, sr, 0xffff, A(xs_trapv_nx), A(xs_trapv_at), -1, 0);
    rend();
    sr = 0x271f;
    xset(sr);
    rbegin(A(xs_trapv_at));
    rin("sr", sr);
    xrun(A(xs_trapv));
    rexc(7, sr, 0xffff, A(xs_trapv_nx), A(xs_trapv_at), -1, 0);
    rend();
    kend("trapv_v_set");

    kbegin(W_TRAPV);
    sr = 0x271d;
    xset(sr);
    rbegin(A(xs_trapv_at));
    rin("sr", sr);
    xrun(A(xs_trapv));
    rnone(0, 0);
    rend();
    kend("trapv_v_clear");
}

/* ------------------------------------------------------------ TRAPcc */
static int cond(int cc, int ccr)
{
    int n = (ccr >> 3) & 1, z = (ccr >> 2) & 1, v = (ccr >> 1) & 1, c = ccr & 1;
    switch (cc) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !c && !z;
    case 3: return c || z;
    case 4: return !c;
    case 5: return c;
    case 6: return !z;
    case 7: return z;
    case 8: return !v;
    case 9: return v;
    case 10: return !n;
    case 11: return n;
    case 12: return n == v;
    case 13: return n != v;
    case 14: return !z && n == v;
    default: return z || n != v;
    }
}

static void t_trapcc(void)
{
    static const char *const ids[3] = { "trapcc_cond", "trapcc_w_cond", "trapcc_l_cond" };
    static const UBYTE quick_ccr[6] = { 0x00, 0x04, 0x0a, 0x11, 0x0f, 0x19 };
    int form, cc, k, nccr;
    if (!GE020) {
        for (form = 0; form < 3; form++)
            skip(ids[form], "TRAPcc is 68020+ (illegal on 68000/010, see illegal_trapcc_000_010)");
        return;
    }
    nccr = AMT_QUICK() ? 6 : 32;
    for (form = 0; form < 3; form++) {
        kbegin(W_TRAPCC);
        for (cc = 0; cc < 16; cc++) {
            ULONG at = A(xs_tcc) + (ULONG)(form * 16 + cc) * 8;
            for (k = 0; k < nccr; k++) {
                int ccr = AMT_QUICK() ? quick_ccr[k] : k;
                UWORD sr = 0x2700 | ccr;
                xset(sr);
                rbegin(at);
                rin("cc", cc);
                rin("sr", sr);
                xrun(at);
                if (cond(cc, ccr))
                    rexc(7, sr, 0xffff, at + 2 + 2 * form, at, -1, 0);
                else
                    rnone(0, 0);
                rend();
            }
        }
        kend(ids[form]);
    }
}

/* ------------------------------------------------------------ CHK */
struct chkin { ULONG v, b; };
static const struct chkin chkw_in[] = {
    { 0, 10 }, { 5, 10 }, { 10, 10 }, { 11, 10 }, { 0xffff, 10 }, { 0x8000, 0x7fff },
    { 0x7fff, 0x7fff }, { 0, 0 }, { 1, 0 }, { 0xffff0005UL, 10 }, { 5, 0xfffd },
    { 0x00017fffUL, 0x00008000UL }, { 0xfffe, 0xffff }, { 0x12340000UL, 0x5678 },
};
static const struct chkin chkl_in[] = {
    { 0, 10 }, { 10, 10 }, { 11, 10 }, { 0xffffffffUL, 10 }, { 0x80000000UL, 0x7fffffffUL },
    { 0x7fffffffUL, 0x7fffffffUL }, { 0x00010000UL, 0xffff }, { 0xffff, 0x00010000UL },
    { 0, 0 }, { 1, 0 }, { 0xfffffffeUL, 0xffffffffUL },
};

/* returns 0 = no trap, 1 = trap N=0, 2 = trap N=1, -1 = ambiguous (skip) */
static int chk_rule(LONG v, LONG b)
{
    if (v < 0 && v > b)
        return -1;
    if (v < 0)
        return 2;
    if (v > b)
        return 1;
    return 0;
}

static void chk_run(ULONG fn, ULONG at, ULONG nx, ULONG v, ULONG b, int lng, int k)
{
    LONG sv = lng ? (LONG)v : (LONG)(WORD)v;
    LONG sb = lng ? (LONG)b : (LONG)(WORD)b;
    int r = chk_rule(sv, sb);
    UWORD sr = (k & 1) ? 0x2707 : 0x2719;
    if (r < 0)
        return;
    xset(sr);
    xr_in[0] = v;
    xr_in[1] = b;
    rbegin(at);
    rin("sr", sr);
    rin("d0", v);
    rin("d1", b);
    xrun(fn);
    if (r)
        rexc(6, (sr & 0xff10) | (r == 2 ? 8 : 0), 0xff18, nx, at, -1, 0);
    else
        rnone(sr & 0xff10, 0xff10);
    rend();
}

static void t_chk(void)
{
    unsigned i;
    int k, nr = AMT_QUICK() ? 4 : 24;

    kbegin(W_CHK);
    for (i = 0; i < sizeof(chkw_in) / sizeof(chkw_in[0]); i++)
        chk_run(A(xs_chkw), A(xs_chkw_at), A(xs_chkw_nx), chkw_in[i].v, chkw_in[i].b, 0, i);
    for (k = 0; k < nr; k++) {
        ULONG v = amt_rand(), b = amt_rand() & 0x7fff;
        if (k & 1)
            v &= 0xffff007fUL;          /* small positive/negative words too */
        chk_run(A(xs_chkw), A(xs_chkw_at), A(xs_chkw_nx), v, b, 0, k);
    }
    kend("chk_w");

    kbegin(W_CHK);
    chk_run(A(xs_chkwi), A(xs_chkwi_at), A(xs_chkwi_nx), 50, 100, 0, 0);
    chk_run(A(xs_chkwi), A(xs_chkwi_at), A(xs_chkwi_nx), 100, 100, 0, 1);
    chk_run(A(xs_chkwi), A(xs_chkwi_at), A(xs_chkwi_nx), 101, 100, 0, 2);
    chk_run(A(xs_chkwi), A(xs_chkwi_at), A(xs_chkwi_nx), 0xffffff9cUL, 100, 0, 3);
    kend("chk_w_imm");

    if (!GE020) {
        skip("chk_l", "CHK.L is 68020+ (illegal on 68000/010, see illegal_chk_l_000_010)");
        return;
    }
    kbegin(W_CHK);
    for (i = 0; i < sizeof(chkl_in) / sizeof(chkl_in[0]); i++)
        chk_run(A(xs_chkl), A(xs_chkl_at), A(xs_chkl_nx), chkl_in[i].v, chkl_in[i].b, 1, i);
    for (k = 0; k < nr; k++) {
        ULONG v = amt_rand(), b = amt_rand() & 0x7fffffffUL;
        if (k & 1)
            v &= 0x8000ffffUL;
        chk_run(A(xs_chkl), A(xs_chkl_at), A(xs_chkl_nx), v, b, 1, k);
    }
    kend("chk_l");
}

/* ------------------------------------------------------------ CHK2/CMP2 */
struct c2 {
    const char *id, *id060;
    UBYTE *fn, *at, *nx;
    UBYTE size, areg, chk;
    ULONG lo, hi;
    ULONG v[8];
    UBYTE nv;
};
static const struct c2 c2_tab[] = {
    { "chk2_l", "unimpl060_chk2_l", xs_chk2l, xs_chk2l_at, xs_chk2l_nx, 4, 0, 1, 0x100, 0x200,
      { 0xff, 0x100, 0x150, 0x200, 0x201, 0x80000000UL, 0x7fffffffUL, 0 }, 8 },
    { "cmp2_l", "unimpl060_cmp2_l", xs_cmp2l, xs_cmp2l_at, xs_cmp2l_nx, 4, 0, 0, 0x100, 0x200,
      { 0xff, 0x100, 0x150, 0x200, 0x201, 0x80000000UL, 0x7fffffffUL, 0 }, 8 },
    { "chk2_b", "unimpl060_chk2_b", xs_chk2b, xs_chk2b_at, xs_chk2b_nx, 1, 0, 1, 0x10, 0x20,
      { 0xffffff15UL, 0x0f, 0x21, 0x10, 0x20, 0x180, 0x1234ff18UL }, 7 },
    { "cmp2_w", "unimpl060_cmp2_w", xs_cmp2w, xs_cmp2w_at, xs_cmp2w_nx, 2, 0, 0, 0x10, 0x100,
      { 0x00010050UL, 0x50, 0x0f, 0x100, 0x8000, 0x10, 0xffff0101UL }, 7 },
    { "chk2_w_areg", "unimpl060_chk2_w_areg", xs_chk2wa, xs_chk2wa_at, xs_chk2wa_nx, 2, 1, 1,
      0x10, 0x100, { 0x50, 0x00010050UL, 0x10, 0x101, 0x100, 0xffff0050UL }, 6 },
};

static void c2_bounds(const struct c2 *t)
{
    UBYTE *b = pool + P_BND;
    if (t->size == 4) {
        *(ULONG *)b = t->lo;
        *(ULONG *)(b + 4) = t->hi;
    } else if (t->size == 2) {
        *(UWORD *)b = (UWORD)t->lo;
        *(UWORD *)(b + 2) = (UWORD)t->hi;
    } else {
        b[0] = (UBYTE)t->lo;
        b[1] = (UBYTE)t->hi;
    }
}

static void t_chk2(void)
{
    unsigned i;
    int k;
    for (i = 0; i < sizeof(c2_tab) / sizeof(c2_tab[0]); i++) {
        const struct c2 *t = &c2_tab[i];
        if (!GE020) {
            skip(t->id, "CHK2/CMP2 are 68020+ (illegal on 68000/010, see illegal_chk2_000_010)");
            skip(t->id060, "68060 only");
            continue;
        }
        c2_bounds(t);
        if (IS060) {
            UWORD sr = 0x2715;
            skip(t->id, "68060: CHK2/CMP2 are unimplemented integer instructions (see unimpl060_*)");
            kbegin(W_UNIMPL060);
            xset(sr);
            xr_ina0 = A(pool + P_BND);
            if (t->areg)
                xr_ina1 = t->v[0];
            else
                xr_in[0] = t->v[0];
            rbegin(A(t->at));
            rin("sr", sr);
            rin("rn", t->v[0]);
            xrun(A(t->fn));
            rexc(61, sr, 0xffff, A(t->at), 0, 0, 0);
            rend();
            kend(t->id060);
            continue;
        }
        skip(t->id060, "68060 only");
        kbegin(W_CHK2);
        for (k = 0; k < t->nv; k++) {
            ULONG m = t->size == 4 ? 0xffffffffUL : t->size == 2 ? 0xffffUL : 0xffUL;
            ULONG val = t->areg ? t->v[k] : (t->v[k] & m);
            int out = val < t->lo || val > t->hi;
            int z = val == t->lo || val == t->hi;
            UWORD sr = (k & 1) ? 0x2700 : 0x271f;
            UWORD esr = (sr & 0xff10) | (z ? 4 : 0) | (out ? 1 : 0);
            xset(sr);
            xr_ina0 = A(pool + P_BND);
            if (t->areg)
                xr_ina1 = t->v[k];
            else
                xr_in[0] = t->v[k];
            rbegin(A(t->at));
            rin("sr", sr);
            rin("rn", t->v[k]);
            rin("lo", t->lo);
            rin("hi", t->hi);
            xrun(A(t->fn));
            if (t->chk && out)
                rexc(6, esr, 0xff15, A(t->nx), A(t->at), -1, 0);
            else
                rnone(esr, 0xff15);
            rend();
        }
        kend(t->id);
    }
}

/* ------------------------------------------------------------ DIV by zero */
struct dv {
    const char *id;
    UBYTE *fn, *at, *nx;
    UBYTE lng, is64;
};
static const struct dv dv_tab[] = {
    { "divu_w_zero", xs_divuw, xs_divuw_at, xs_divuw_nx, 0, 0 },
    { "divs_w_zero", xs_divsw, xs_divsw_at, xs_divsw_nx, 0, 0 },
    { "divu_w_imm_zero", xs_divuwi, xs_divuwi_at, xs_divuwi_nx, 0, 0 },
    { "divs_w_imm_zero", xs_divswi, xs_divswi_at, xs_divswi_nx, 0, 0 },
    { "divu_l_zero", xs_divul, xs_divul_at, xs_divul_nx, 1, 0 },
    { "divs_l_zero", xs_divsl, xs_divsl_at, xs_divsl_nx, 1, 0 },
    { "divul_l_zero", xs_divull, xs_divull_at, xs_divull_nx, 1, 0 },
    { "divsl_l_zero", xs_divsll, xs_divsll_at, xs_divsll_nx, 1, 0 },
    { "divu_l64_zero", xs_divul64, xs_divul64_at, xs_divul64_nx, 1, 1 },
    { "divs_l64_zero", xs_divsl64, xs_divsl64_at, xs_divsl64_nx, 1, 1 },
    { "divu_l_imm_zero", xs_divuli, xs_divuli_at, xs_divuli_nx, 1, 0 },
};

static void t_div0(void)
{
    static const UWORD srs[3] = { 0x271f, 0x2700, 0x2711 };
    static const ULONG d0s[3] = { 0x12345678UL, 0x80000000UL, 0 };
    unsigned i;
    int k;
    for (i = 0; i < sizeof(dv_tab) / sizeof(dv_tab[0]); i++) {
        const struct dv *t = &dv_tab[i];
        if (t->lng && !GE020) {
            skip(t->id, "long divide is 68020+ (illegal on 68000/010, see illegal_muldiv_l_000_010)");
            continue;
        }
        if (t->is64 && IS060) {
            skip(t->id, "68060: the 64-bit dividend forms are unimplemented integer instructions (see unimpl060_div*_l64)");
            continue;
        }
        kbegin(W_DIV0);
        for (k = 0; k < 3; k++) {
            UWORD sr = srs[k];
            xset(sr);
            xr_in[0] = d0s[k];
            xr_in[1] = 0;
            xr_in[2] = 0x0badf00dUL;
            rbegin(A(t->at));
            rin("sr", sr);
            rin("d0", d0s[k]);
            rin("d1", 0);
            rin("d2", 0x0badf00dUL);
            xrun(A(t->fn));
            rexc(5, sr & 0xff10, 0xff11, A(t->nx), A(t->at), -1, 0);
            if (xr_vec == 5) {
                rcmp("d0", d0s[k], xr_regs[0], T_H8);
                rcmp("d2", 0x0badf00dUL, xr_regs[2], T_H8);
            }
            rend();
        }
        kend(t->id);
    }
}

/* ------------------------------------------------------------ 060 unimplemented */
struct ui {
    const char *id;
    UBYTE *fn, *at;
    UBYTE mem;                  /* a0/a1 point at the CAS operand area */
    UBYTE odd;                  /* a0 odd */
};
static const struct ui ui_tab[] = {
    { "unimpl060_movep_w", xs_movepw, xs_movepw_at, 1, 0 },
    { "unimpl060_movep_l", xs_movepl, xs_movepl_at, 1, 0 },
    { "unimpl060_cas2_w", xs_cas2w, xs_cas2w_at, 1, 0 },
    { "unimpl060_cas2_l", xs_cas2l, xs_cas2l_at, 1, 0 },
    { "unimpl060_cas_misaligned", xs_casw, xs_casw_at, 1, 1 },
    { "unimpl060_mulu_l64", xs_mulul64, xs_mulul64_at, 0, 0 },
    { "unimpl060_muls_l64", xs_mulsl64, xs_mulsl64_at, 0, 0 },
    { "unimpl060_divu_l64", xs_divul64, xs_divul64_at, 0, 0 },
    { "unimpl060_divs_l64", xs_divsl64, xs_divsl64_at, 0, 0 },
};

static void t_unimpl060(void)
{
    static const ULONG din[4] = { 0x12345678UL, 7, 0, 0x00001111UL };
    unsigned i;
    int r, j;
    for (i = 0; i < sizeof(ui_tab) / sizeof(ui_tab[0]); i++) {
        const struct ui *t = &ui_tab[i];
        UWORD sr = 0x2715;
        ULONG a0, a1, before[4];
        if (!IS060) {
            skip(t->id, "68060 only (implemented on the other CPUs)");
            continue;
        }
        kbegin(W_UNIMPL060);
        for (j = 0; j < 16; j++)
            pool[P_CAS + j] = (UBYTE)(0x40 + j);
        for (j = 0; j < 4; j++)
            before[j] = ((ULONG *)(pool + P_CAS))[j];
        a0 = A(pool + P_CAS) + (t->odd ? 1 : 0);
        a1 = A(pool + P_CAS) + 8;
        xset(sr);
        for (r = 0; r < 4; r++)
            xr_in[r] = din[r];
        if (t->mem) {
            xr_ina0 = a0;
            xr_ina1 = a1;
            xr_in[0] = 0x40414243UL;    /* CAS compare operands equal memory */
            xr_in[1] = 0x48494a4bUL;
        }
        rbegin(A(t->at));
        rin("sr", sr);
        rin("d0", xr_in[0]);
        rin("d1", xr_in[1]);
        /* a0/a1 as offsets from the pool (0 = not used by this form) */
        rin("a0_pool_off", xr_ina0 ? xr_ina0 - A(pool) : 0);
        xrun(A(t->fn));
        rexc(61, sr, 0xffff, A(t->at), 0, 0, 0);
        if (xr_vec == 61) {
            for (r = 0; r < 4; r++)
                rcmp("dreg", xr_in[r], xr_regs[r], T_H8);
            rcmp("a0_pool_off", xr_ina0 - A(pool), xr_regs[8] - A(pool), T_H8);
            rcmp("a1_pool_off", xr_ina1 - A(pool), xr_regs[9] - A(pool), T_H8);
        }
        for (j = 0; j < 4; j++)
            rcmp("mem", before[j], ((ULONG *)(pool + P_CAS))[j], T_H8);
        rend();
        kend(t->id);
    }
}

/* ------------------------------------------------------------ illegal */
struct il { UWORD op; UBYTE fams; UBYTE vec; const char *id; };
static const struct il ill_tab[] = {
    { 0x4afc, FALL, 4, "illegal_4afc" },
    { 0x41c0, FALL, 4, "illegal_lea_dreg_41c0" },
    { 0x4ec0, FALL, 4, "illegal_jmp_dreg_4ec0" },
    { 0x4ec8, FALL, 4, "illegal_jmp_areg_4ec8" },
    { 0x4e7c, FALL, 4, "illegal_4e7c" },
    { 0x4e7d, FALL, 4, "illegal_4e7d" },
    { 0x4e7e, FALL, 4, "illegal_4e7e" },
    { 0x4e7f, FALL, 4, "illegal_4e7f" },
    { 0x4afa, FALL, 4, "illegal_tas_pcrel_4afa" },
    { 0x4afb, FALL, 4, "illegal_tas_pcidx_4afb" },
    { 0x4e74, F000, 4, "illegal_rtd_000" },
    { 0x4e7a, F000, 4, "illegal_movec_000" },
    { 0x42c0, F000, 4, "illegal_move_ccr_000" },
    { 0x0e90, F000, 4, "illegal_moves_000" },
    { 0x49c0, F000 | F010, 4, "illegal_extb_000_010" },
    { 0x4808, F000 | F010, 4, "illegal_link_l_000_010" },
    { 0x4101, F000 | F010, 4, "illegal_chk_l_000_010" },
    { 0x4c41, F000 | F010, 4, "illegal_muldiv_l_000_010" },
    { 0x50fc, F000 | F010, 4, "illegal_trapcc_000_010" },
    { 0x00d0, F000 | F010, 4, "illegal_chk2_000_010" },
    { 0x06c0, FALL & ~F020, 4, "illegal_rtm_not_020" },
    { 0xa000, FALL, 10, "line_a_a000" },
    { 0xafff, FALL, 10, "line_a_afff" },
    { 0xffff, FALL, 11, "line_f_ffff" },
};

static void t_illegal(void)
{
    unsigned i, n = sizeof(ill_tab) / sizeof(ill_tab[0]);
    if ((A(xs_ill_end) - A(xs_ill)) / 8 != n) {
        amt_check32("illegal_table", "entries", n, (A(xs_ill_end) - A(xs_ill)) / 8,
                    "exceptions_asm.s xs_ill and ill_tab[] differ (build error)");
        return;
    }
    for (i = 0; i < n; i++) {
        const struct il *t = &ill_tab[i];
        ULONG at = A(xs_ill) + 8 * i;
        UWORD sr = 0x2704;
        if (!(t->fams & FBIT)) {
            skip(t->id, "a valid instruction on this CPU");
            continue;
        }
        kbegin(W_ILL);
        xset(sr);
        /* a0/a1 at pool scratch: a wrongly decoded MOVES.L/CHK2 ($0e90 or
         * $00d0 with the $4afc filler as extension word) accesses (a0) */
        xr_ina0 = xr_ina1 = A(pool + P_CAS);
        rbegin(at);
        rin("sr", sr);
        rin("op", t->op);
        rcmp("table_op", t->op, *(UWORD *)at, T_H4);
        xrun(at);
        rexc(t->vec, sr, 0xffff, at, 0, 0, 0);
        rend();
        kend(t->id);
    }
}

static void t_fline_fpu(void)
{
    UWORD sr = 0x2700;
    ULONG at = A(xs_fnop_at), pc;
    if (AMT_FPU()) {
        skip("fline_fpu_nofpu", "FPU present: FPU exceptions belong to the fpu module");
        return;
    }
    kbegin(W_FLINE_NOFPU);
    xset(sr);
    rbegin(at);
    rin("sr", sr);
    xrun(A(xs_fnop));
    if (fam >= 4) {
        rexc(11, sr, 0xffff, A(xs_fnop_nx), 0, 4, 0);
        if (xr_vec == 11)
            rcmpo("fault_pc_off", at, frl(xr_frame, 6));
        (void)pc;
    } else {
        rexc(11, sr, 0xffff, at, 0, 0, 0);
    }
    rend();
    kend("fline_fpu_nofpu");
}

/* ------------------------------------------------------------ privilege */
static void urun(const char *id, const char *why, ULONG fn, ULONG at, ULONG nx,
                 UWORD vec, ULONG pc)
{
    UWORD sr = 0x2704, usr = 0x0704;
    kbegin(why);
    xset(sr);
    xr_usp = A(pool + P_USTK);
    xr_ina0 = A(pool + P_CAS);
    rbegin(at);
    rin("user_sr", usr);
    xrun(fn);
    rexc(vec, usr, 0xffff, pc, 0, 0, 0);
    (void)nx;
    rend();
    kend(id);
}

#define UR(id, why, n, vec, pc) urun(id, why, A(xs_##n), A(xs_##n##_at), A(xs_##n##_nx), vec, pc)

static void t_priv(void)
{
    UR("priv_move_to_sr", W_PRIV, umovesr, 8, A(xs_umovesr_at));
    UR("priv_andi_sr", W_PRIV, uandisr, 8, A(xs_uandisr_at));
    UR("priv_ori_sr", W_PRIV, uorisr, 8, A(xs_uorisr_at));
    UR("priv_eori_sr", W_PRIV, ueorisr, 8, A(xs_ueorisr_at));
    UR("priv_move_usp", W_PRIV, umoveusp, 8, A(xs_umoveusp_at));
    UR("priv_rte_user", W_PRIV, urte, 8, A(xs_urte_at));
    UR("user_trap_frame", W_UTRAP, utrap, 35, A(xs_utrap_nx));
    if (IS000) {
        /* legal: runs on to the ILLEGAL escape at _nx; d0 = user SR */
        UR("move_from_sr_user_000", W_MFSR000, umovefsr, 4, A(xs_umovefsr_nx));
        skip("priv_move_from_sr_user", "68010+ (not privileged on the 68000, see move_from_sr_user_000)");
        UR("illegal_movec_user_000", W_MOVEC000, umovec, 4, A(xs_umovec_at));
        skip("priv_movec_user", "68010+ (MOVEC is illegal on the 68000, see illegal_movec_user_000)");
        skip("priv_moves_user", "68010+ (MOVES is illegal on the 68000, see illegal_moves_000)");
    } else {
        skip("move_from_sr_user_000", "68000 only");
        UR("priv_move_from_sr_user", W_PRIV, umovefsr, 8, A(xs_umovefsr_at));
        skip("illegal_movec_user_000", "68000 only");
        UR("priv_movec_user", W_PRIV, umovec, 8, A(xs_umovec_at));
        UR("priv_moves_user", W_PRIV, umoves, 8, A(xs_umoves_at));
    }
}

/* the 68000 MOVE from SR result: d0 at the escape */
static void t_mfsr000_value(void)
{
    if (!IS000) {
        skip("move_from_sr_user_000_value", "68000 only");
        return;
    }
    kbegin(W_MFSR000);
    xset(0x2704);
    xr_usp = A(pool + P_USTK);
    rbegin(A(xs_umovefsr_at));
    xrun(A(xs_umovefsr));
    rcmp("vec", 4, xr_vec, T_INT);
    rcmp("d0", 0x0704, xr_regs[0] & 0xffff, T_H4);
    rend();
    kend("move_from_sr_user_000_value");
}

/* ------------------------------------------------------------ address error */
static int in_range(ULONG v, ULONG lo, ULONG hi)
{
    return v >= lo && v <= hi;
}

static void ae_run(const char *id, ULONG fn, ULONG at, ULONG addr, int rw, int fc,
                   int ifetch, int user)
{
    UWORD sr = 0x2704, ssr = user ? 0x0704 : 0x2704;
    UWORD srm = (rw && !ifetch) ? 0xff1f : 0xff00;
    ULONG pc;
    int pcok;
    kbegin(IS000 ? W_AE000 : IS010 ? W_AE010 : fam <= 3 ? W_AE020 : W_AE040);
    xset(sr);
    xr_usp = A(pool + P_USTK);
    xr_ina0 = addr;
    xr_in[1] = 0x11223344UL;
    rbegin(at);
    rin("a0_off", addr - at);       /* offsets from the instruction under test */
    rin("sr", sr);
    xrun(fn);
    if (xr_vec)
        rframe_from(xr_frame, 16);
    R.grp0 = IS000;
    R.pcrule = IS000 ? "68000 group 0: stacked PC within insn_addr+2 .. insn_addr+10 (for the odd "
                       "JMP also target-3 .. target+5); pc_off_window_lo shows insn+2 vs the PC"
                     : "68010 format $8: stacked PC within insn_addr .. insn_addr+10 (for the odd "
                       "JMP also target-3 .. target+5)";
    if (rcmp("vec", 3, xr_vec, T_INT)) {
        if (GE020) {
            /* 68020+: only an instruction fetch from an odd address takes an
             * address error. 020/030: the bus fault frame, short $A or long
             * $B (68020UM 6.1.2, 68030UM 8.1.2 and 8.2.1: "the same frame as
             * a bus error"); 040/060: format $2, the address field holding
             * the odd address that was fetched (68040UM 8.2.2, 68060UM 8.2.2) */
            int gf = xr_frame[3] >> 12, ef;
            if (fam <= 3) {
                ef = gf == 0xb ? 0xb : 0xa;
                rcmp("format_a_or_b", 1, gf == 0xa || gf == 0xb, T_INT);
            } else {
                ef = 2;
                rcmp("format", 2, gf, T_INT);
            }
            rcmp("frame_size", fsize(ef), xr_spbase - xr_ssp, T_INT);
            rcmp("vector_offset", 0x00c, xr_frame[3] & 0x0fff, T_H4);
            if (ef == 2) {
                /* the odd target, or the target with bit 0 cleared: the
                 * 040/060 UM text could not be confirmed here (the review
                 * marks it "check"), and an instruction fetch has no A0 on
                 * these buses, so both readings are accepted */
                ULONG g = frl(xr_frame, 4);
                rcmpo("fault_addr_off", addr, g == (addr & ~1UL) ? addr : g);
            }
            rcmp("sr", ssr & srm, xr_frame[0] & srm, T_H4);
            pc = frl(xr_frame, 1);
            rcmp("handler_sr", (xr_frame[0] | 0x2000) & 0x3fff, xr_hsr, T_H4);
        } else if (IS000) {
            rcmp("frame_size", 14, xr_spbase - xr_ssp, T_INT);
            rcmp("status", (rw << 4) | fc, xr_frame[0] & 0x1f, T_H4);
            rcmpo("access_addr_off", addr, frl(xr_frame, 1));
            rcmp("ir", *(UWORD *)at, xr_frame[3], T_H4);
            rcmp("sr", ssr & srm, xr_frame[4] & srm, T_H4);
            pc = frl(xr_frame, 5);
            rcmp("handler_sr", (xr_frame[4] | 0x2000) & 0x3fff, xr_hsr, T_H4);
        } else {
            UWORD ssw = (ifetch ? 0x2000 : rw ? 0x1000 : 0) | (rw << 8) | fc;
            rcmp("frame_size", 58, xr_spbase - xr_ssp, T_INT);
            rcmp("fv", 0x800c, xr_frame[3], T_H4);
            rcmp("ssw", ssw, xr_frame[4] & 0x3107, T_H4);
            rcmpo("fault_addr_off", addr, frl(xr_frame, 5));
            rcmp("sr", ssr & srm, xr_frame[0] & srm, T_H4);
            pc = frl(xr_frame, 1);
            rcmp("handler_sr", (xr_frame[0] | 0x2000) & 0x3fff, xr_hsr, T_H4);
        }
        /* the stacked PC is only loosely defined: 2..10 bytes past the
         * instruction (68010: from the instruction on); for an odd JMP
         * target the PC may also be near the target */
        pcok = in_range(pc, at + (IS000 ? 2 : 0), at + 10) ||
               (ifetch && in_range(pc, addr - 3, addr + 5));
        if (!pcok)
            rcmpo("pc_off_window_lo", at + 2, pc);
    }
    rend();
    kend(id);
}

static void t_aerr(void)
{
    static const char *const ids[6] = {
        "addr_err_read_word", "addr_err_read_long", "addr_err_write_word",
        "addr_err_write_long", "addr_err_user_read", "addr_err_jmp_odd"
    };
    ULONG odd = A(pool + P_AE) + 1;
    int i;
    if (GE020) {
        /* data may be misaligned on 68020+ (see misaligned_data_020); an
         * instruction fetch from an odd address still takes vector 3 */
        for (i = 0; i < 5; i++)
            skip(ids[i], "68020+: misaligned data accesses do not take an address error (see "
                         "misaligned_data_020); only addr_err_jmp_odd applies");
        ae_run(ids[5], A(xs_aejmp), A(xs_aejmp_at), A(xs_aetgt) + 1, 1, 6, 1, 0);
        return;
    }
    ae_run(ids[0], A(xs_aerw), A(xs_aerw_at), odd, 1, 5, 0, 0);
    ae_run(ids[1], A(xs_aerl), A(xs_aerl_at), odd, 1, 5, 0, 0);
    ae_run(ids[2], A(xs_aeww), A(xs_aeww_at), odd, 0, 5, 0, 0);
    ae_run(ids[3], A(xs_aewl), A(xs_aewl_at), odd, 0, 5, 0, 0);
    ae_run(ids[4], A(xs_uaerr), A(xs_uaerr_at), odd, 1, 1, 0, 1);
    ae_run(ids[5], A(xs_aejmp), A(xs_aejmp_at), A(xs_aetgt) + 1, 1, 6, 1, 0);
}

static ULONG rd32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static void t_misaligned(void)
{
    UBYTE model[32];
    UBYTE *m = pool + P_MIS;
    ULONG d2 = 0xa1b2c3d4UL, d4 = 0x0000e5f6UL;
    int i;
    if (!GE020) {
        skip("misaligned_data_020", "68020+ only (68000/010 take an address error, see addr_err_*)");
        return;
    }
    for (i = 0; i < 32; i++)
        model[i] = m[i] = (UBYTE)(0x10 + i * 3);
    kbegin(W_MIS);
    xset(0x2700);
    xr_ina0 = A(m) + 1;
    xr_in[2] = d2;
    xr_in[4] = d4;
    rbegin(A(xs_mis));
    rin("a0_off", 1);
    xrun(A(xs_mis));
    /* model: d0 = [1..4], d1 = [3..4], [5..8] = d2, d3 = [5..8],
     * [11..12] = d4.w, d5 = [9..12] */
    {
        ULONG e0 = rd32(model + 1), e1 = ((ULONG)model[3] << 8) | model[4], e3, e5;
        model[5] = (UBYTE)(d2 >> 24); model[6] = (UBYTE)(d2 >> 16);
        model[7] = (UBYTE)(d2 >> 8);  model[8] = (UBYTE)d2;
        e3 = rd32(model + 5);
        model[11] = (UBYTE)(d4 >> 8); model[12] = (UBYTE)d4;
        e5 = rd32(model + 9);
        if (rcmp("vec", 0, xr_vec, T_INT)) {
            rcmp("d0", e0, xr_out[0], T_H8);
            rcmp("d1_w", e1, xr_out[1] & 0xffff, T_H4);
            rcmp("d3", e3, xr_out[3], T_H8);
            rcmp("d5", e5, xr_out[5], T_H8);
            for (i = 0; i < 16; i += 4)
                rcmp("mem", rd32(model + i), rd32(m + i), T_H8);
        } else {
            rframe_from(xr_frame, 12);
        }
    }
    rend();
    kend("misaligned_data_020");
}

/* ------------------------------------------------------------ trace */
struct tev { UWORD vec, sr; ULONG pc, addr; };

static int tmatch(ULONG ret, const struct tev *ex, int n)
{
    int i;
    if (ret || xr_nlog != n)
        return 0;
    for (i = 0; i < n; i++) {
        volatile struct xlog *l = &xr_log[i];
        int fmt = deffmt(ex[i].vec);
        UWORD fv = (UWORD)((fmt << 12) | (ex[i].vec * 4));
        if (!(l->vec == ex[i].vec && l->f[0] == ex[i].sr && frl(l->f, 1) == ex[i].pc &&
              (IS000 || l->f[3] == fv) &&
              (!(GE020 && fmt == 2 && ex[i].addr) || frl(l->f, 4) == ex[i].addr)))
            return 0;
    }
    return 1;
}

static void tdump(void)
{
    int i;
    R.nev = xr_nlog < 12 ? xr_nlog : 12;
    for (i = 0; i < R.nev; i++) {
        volatile struct xlog *l = &xr_log[i];
        R.evd[i][0] = l->vec;
        R.evd[i][1] = l->f[0];
        R.evd[i][2] = (long)(frl(l->f, 1) - R.at);
        R.evd[i][3] = (GE020 && (l->f[3] >> 12) == 2) ? (long)(frl(l->f, 4) - R.at) : 0;
    }
}

static void tcheck(ULONG ret, const struct tev *ex, int n)
{
    int i;
    R.pcrule = "trace frames: PC = the next instruction to execute, addr (020+) = the traced "
               "instruction; non-trace events as their own rule; ctx.events lists every logged "
               "exception";
    if (!tmatch(ret, ex, n))
        tdump();
    rcmp("ret_vec", 0, ret, T_INT);
    rcmp("events", n, xr_nlog, T_INT);
    for (i = 0; i < n && i < xr_nlog; i++) {
        volatile struct xlog *l = &xr_log[i];
        int fmt = deffmt(ex[i].vec);
        UWORD fv = (UWORD)((fmt << 12) | (ex[i].vec * 4));
        int ok = l->vec == ex[i].vec && l->f[0] == ex[i].sr && frl(l->f, 1) == ex[i].pc &&
                 (IS000 || l->f[3] == fv) &&
                 (!(GE020 && fmt == 2 && ex[i].addr) || frl(l->f, 4) == ex[i].addr);
        if (!ok) {
            R.ev = i;
            rframe_from(l->f, 12);
            rcmp("ev_vec", ex[i].vec, l->vec, T_INT);
            rcmp("ev_sr", ex[i].sr, l->f[0], T_H4);
            rcmpo("ev_pc_off", ex[i].pc, frl(l->f, 1));
            if (!IS000)
                rcmp("ev_fv", fv, l->f[3], T_H4);
            if (GE020 && fmt == 2 && ex[i].addr)
                rcmpo("ev_addr_off", ex[i].addr, frl(l->f, 4));
            return;
        }
    }
    if (xr_nlog > n && n < XLOGMAX) {
        /* report the first unexpected extra event */
        volatile struct xlog *l = &xr_log[n];
        R.ev = n;
        rframe_from(l->f, 12);
        rcmp("extra_vec", 0, l->vec, T_INT);
        rcmpo("extra_pc_off", R.at, frl(l->f, 1));
    }
}

static ULONG trace_run(ULONG fn, ULONG stop, UWORD skipvec)
{
    ULONG r;
    xset(0x2700);
    xr_mode = 1;
    xr_stop_pc = stop;
    xr_skipvec = skipvec;
    r = xrun(fn);
    xr_mode = 0;
    return r;
}

static int tr1_expect(struct tev *e)
{
    static UBYTE *const pcs[9] = { xs_tr1_a, xs_tr1_b, xs_tr1_c, xs_tr1_e, xs_tr1_sub,
                                   xs_tr1_s1, xs_tr1_f, xs_tr1_g, xs_tr1_h };
    int i;
    for (i = 0; i < 8; i++) {
        e[i].vec = 9;
        e[i].sr = i == 7 ? 0x2700 : 0xa700;
        e[i].pc = A(pcs[i + 1]);
        e[i].addr = A(pcs[i]);
    }
    return 8;
}

static void t_trace(void)
{
    struct tev e[8];
    ULONG r;
    int n;

    kbegin(W_TRACE1);
    n = tr1_expect(e);
    rbegin(A(xs_tr1));
    r = trace_run(A(xs_tr1), 0, 0);
    tcheck(r, e, n);
    rend();
    kend("trace_t1_single_step");

    if (!GE020 || IS060) {
        skip("trace_t0_flow", IS060 ? "68060 has no change-of-flow trace (T0)"
                                    : "T0 (trace on change of flow) is 68020-68040 only");
    } else {
        /* 68020/030: BRA, JSR, RTS, BRA. 68040: NOP is in the
         * change-of-flow list too (68040UM 8.2.6) */
        int k = 0;
        kbegin(W_TRACE0);
        e[k].pc = A(xs_tr0_d); e[k++].addr = A(xs_tr0_b);
        e[k].pc = A(xs_tr0_sub); e[k++].addr = A(xs_tr0_e);
        if (fam == 4) {
            e[k].pc = A(xs_tr0_s1); e[k++].addr = A(xs_tr0_sub);
        }
        e[k].pc = A(xs_tr0_f); e[k++].addr = A(xs_tr0_s1);
        if (fam == 4) {
            e[k].pc = A(xs_tr0_g); e[k++].addr = A(xs_tr0_f);
        }
        e[k].pc = A(xs_tr0_end); e[k++].addr = A(xs_tr0_g);
        for (n = 0; n < k; n++) {
            e[n].vec = 9;
            e[n].sr = 0x6700;
        }
        rbegin(A(xs_tr0));
        r = trace_run(A(xs_tr0), A(xs_tr0_end), 0);
        tcheck(r, e, k);
        rend();
        kend("trace_t0_flow");
    }

    /* 68000-030: the TRAP's exception processing, then the trace exception
     * before the first instruction of the TRAP handler: the trace handler
     * runs first with PC = TRAP handler (M68000PRM 6.3.8 / 68020UM 6.1.8
     * tracing: an instruction that traps is traced after its exception
     * processing, before the handler's first instruction). 68040/060: the
     * trace is not reported at all (68040UM 8.3, 68060UM 8.2.6). */
    kbegin(fam >= 4 ? W_TRTRAP4 : W_TRTRAP);
    rbegin(A(xs_trt_at));
    r = trace_run(A(xs_trt), 0, 0);
    if (fam >= 4) {
        e[0].vec = 37; e[0].sr = 0xa700; e[0].pc = A(xs_trt_nx); e[0].addr = 0;
        e[1].vec = 9; e[1].sr = 0x2700; e[1].pc = A(xs_trt_n2); e[1].addr = A(xs_trt_nx);
        n = 2;
    } else {
        e[0].vec = 9; e[0].sr = 0x2700; e[0].pc = amt_vbr()[37]; e[0].addr = A(xs_trt_at);
        e[1].vec = 37; e[1].sr = 0xa700; e[1].pc = A(xs_trt_nx); e[1].addr = 0;
        e[2].vec = 9; e[2].sr = 0x2700; e[2].pc = A(xs_trt_n2); e[2].addr = A(xs_trt_nx);
        n = 3;
    }
    tcheck(r, e, n);
    rend();
    kend("trace_trap_order");

    kbegin(W_TRILL);
    e[0].vec = 4; e[0].sr = 0xa700; e[0].pc = A(xs_tri_at); e[0].addr = 0;
    e[1].vec = 9; e[1].sr = 0x2700; e[1].pc = A(xs_tri_n2); e[1].addr = A(xs_tri_nx);
    rbegin(A(xs_tri_at));
    r = trace_run(A(xs_tri), 0, 4);
    tcheck(r, e, 2);
    rend();
    kend("trace_illegal_not_traced");
}

/* ------------------------------------------------------------ nested */
static void t_nested(void)
{
    UWORD sr = 0x2711;
    ULONG size = IS000 ? 6 : 8, r;
    int i;
    amt_vec_fn o1, o2;
    kbegin(W_NEST);
    xset(sr);
    o1 = amt_vec_set(33, xh_nest1);
    o2 = amt_vec_set(34, xh_nest2);
    rbegin(A(xs_nest_at));
    rin("sr", sr);
    r = xrun(A(xs_nest));
    amt_vec_set(33, o1);
    amt_vec_set(34, o2);
    rframe_from(xn_f2, 8);
    rcmp("ret_vec", 0, r, T_INT);
    rcmp("f1_sr", sr, xn_f1[0], T_H4);
    rcmpo("f1_pc_off", A(xs_nest_nx), frl(xn_f1, 1));
    rcmp("ssp1_depth", size, xr_spbase - xn_ssp1, T_INT);
    rcmp("hsr1", sr, xn_hsr1, T_H4);
    rcmp("f2_sr", 0x2708, xn_f2[0], T_H4);
    rcmpo("f2_pc_off", A(xh_nest1_nx), frl(xn_f2, 1));
    rcmp("ssp2_depth", size, xn_ssp1 - xn_ssp2, T_INT);
    rcmp("hsr2", 0x2708, xn_hsr2, T_H4);
    if (!IS000) {
        rcmp("f1_fv", 0x0084, xn_f1[3], T_H4);
        rcmp("f2_fv", 0x0088, xn_f2[3], T_H4);
    }
    rcmp("ssp1_after", xn_ssp1, xn_ssp1b, T_H8);
    for (i = 0; i < (int)(size / 2); i++)
        rcmp("f1_intact", xn_f1[i], xn_f1b[i], T_H4);
    rcmp("sr_after", sr, xr_out[7] & 0xffff, T_H4);
    rend();
    kend("nested_trap");
}

/* ------------------------------------------------------------ RTE */
static void rte_ok(const char *id, UWORD fw, int extra, ULONG esp)
{
    ULONG r;
    kbegin(IS000 ? W_RTE000 : W_RTEOK);
    xset(0x2700);
    xr_in[0] = fw;
    xr_in[1] = extra;
    xr_in[2] = 0x2715;
    rbegin(A(xs_rte_at));
    rin("fv", fw);
    rin("extra_words", extra);
    r = xrun(A(xs_rte));
    if (r)
        rframe_from(xr_frame, 12);
    if (rcmp("vec", 0, r, T_INT)) {
        rcmp("sr_after", 0x2715, xr_out[7] & 0xffff, T_H4);
        rcmp("sp_delta", esp, xr_out[6], T_INT);
    }
    rend();
    kend(id);
}

static void t_rte(void)
{
    /* invalid formats per family (010, 020, 030, 040, 060) */
    static const UBYTE bad010[] = { 1, 2, 3, 4, 7, 9, 10, 11, 15 };
    static const UBYTE bad020[] = { 3, 4, 5, 6, 7, 8, 12, 13, 14, 15 };
    static const UBYTE bad040[] = { 5, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const UBYTE bad060[] = { 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    const UBYTE *bad;
    int nbad, i;
    ULONG r;

    if (IS000) {
        rte_ok("rte_000_pops_6", 0x0000, 0, (ULONG)-2);
        skip("rte_format_0", "68010+ (68000 has no format word, see rte_000_pops_6)");
    } else {
        skip("rte_000_pops_6", "68000 only");
        rte_ok("rte_format_0", 0x0000, 0, 0);
    }
    if (GE020)
        rte_ok("rte_format_2", 0x2018, 2, 0);
    else
        skip("rte_format_2", "format $2 is 68020+");
    if (fam >= 4)
        rte_ok("rte_format_3", 0x3000, 2, 0);
    else
        skip("rte_format_3", "format $3 is 68040/68060");
    /* format $4: the 68060 (access fault / unimplemented FP frames) and the
     * 68LC040/68EC040, which stack it for an unimplemented FP instruction
     * (68040UM 8.2 stack frame formats: $4 is EC040/LC040 only; the F-line
     * frame tested by fline_no_fpu). The
     * full 68040 never generates it and the manual does not list it as
     * valid or invalid there, so it is in neither list for that CPU. */
    if (IS060 || cpu == AMT_CPU_68LC040 || cpu == AMT_CPU_68EC040)
        rte_ok("rte_format_4", 0x4000, 4, 0);
    else
        skip("rte_format_4", fam == 4 ? "the full 68040 never stacks format $4 (only the LC/EC040 "
                                         "do); its RTE behaviour for $4 is not specified"
                                       : "format $4 is 68LC040/68EC040/68060 only");

    /* format $1 throwaway, then the format $0 frame below it */
    if (GE020 && !IS060) {
        kbegin(W_RTEOK);
        xset(0x2700);
        xr_in[2] = 0x2715;
        xr_in[3] = 0x2700;
        rbegin(A(xs_rte1_at));
        r = xrun(A(xs_rte1));
        if (r)
            rframe_from(xr_frame, 12);
        if (rcmp("vec", 0, r, T_INT)) {
            rcmp("sr_after", 0x2715, xr_out[7] & 0xffff, T_H4);
            rcmp("sp_delta", 0, xr_out[6], T_INT);
        }
        rend();
        kend("rte_format_1");
    } else {
        skip("rte_format_1", IS060 ? "68060 has no throwaway frame (format $1 is a format error there)"
                                   : "format $1 is 68020-68040");
    }

    if (IS000) {
        skip("rte_format_invalid", "68010+ (68000 RTE has no format word)");
        return;
    }
    switch (fam) {
    case 1: bad = bad010; nbad = sizeof(bad010); break;
    case 2: case 3: bad = bad020; nbad = sizeof(bad020); break;
    case 4: bad = bad040; nbad = sizeof(bad040); break;
    default: bad = bad060; nbad = sizeof(bad060); break;
    }
    kbegin(W_RTEBAD);
    for (i = 0; i < nbad; i++) {
        UWORD fw = (UWORD)(bad[i] << 12);
        xset(0x2700);
        xr_in[0] = fw;
        xr_in[1] = 8;
        xr_in[2] = 0x2715;
        xr_in[4] = 1;
        rbegin(A(xs_rte_at));
        rin("fv", fw);
        xrun(A(xs_rte));
        rexc(14, 0x2700, IS010 ? 0xfff1 : 0xffff, A(xs_rte_at), 0, 0, X_NOSIZE);
        rend();
    }
    kend("rte_format_invalid");
}

/* ------------------------------------------------------------ bus error */
static void t_berr(void)
{
    const ULONG addr = 0x7f000000UL;
    int i;
    ULONG r, fa = 0, fmt;
    if (!(amt_par.flags & AMT_F_UNSAFE)) {
        skip("bus_error_unmapped_read", "unsafe tests disabled");
        return;
    }
    if (amt_probe_result.addr24 || !GE020) {
        amt_case("bus_error_unmapped_read", AMT_ST_UNTESTABLE,
                 "24-bit bus: no address range is guaranteed to raise a bus error on read");
        return;
    }
    for (i = 0; i < amt_probe_result.n_boards; i++) {
        const struct amt_probe_board *b = &amt_probe_result.boards[i];
        if (addr >= b->addr && addr - b->addr < b->size) {
            amt_case("bus_error_unmapped_read", AMT_ST_UNTESTABLE, "a board decodes $7F000000");
            return;
        }
    }
    for (i = 0; i < amt_probe_result.n_mem; i++) {
        const struct amt_probe_mem *m = &amt_probe_result.mem[i];
        if (addr >= m->lower && addr < m->upper) {
            amt_case("bus_error_unmapped_read", AMT_ST_UNTESTABLE, "RAM at $7F000000");
            return;
        }
    }
    xset(0x2700);
    xr_ina0 = addr;
    r = xrun(A(xs_berr));
    if (r == 0) {
        amt_case("bus_error_unmapped_read", AMT_ST_UNTESTABLE,
                 "the read of $7F000000 (Zorro III space, nothing configured there) completed: "
                 "this machine does not time out unmapped reads");
        return;
    }
    kbegin(W_BERR);
    rbegin(A(xs_berr_at));
    rin("addr", addr);
    rframe_from(xr_frame, 16);
    if (rcmp("vec", 2, r, T_INT)) {
        fmt = xr_frame[3] >> 12;
        if (fam <= 3) {
            if (fmt != 10)
                rcmp("format", 11, fmt, T_INT);
            fa = frl(xr_frame, 8);
        } else if (fam == 4) {
            rcmp("format", 7, fmt, T_INT);
            fa = frl(xr_frame, 10);
        } else {
            rcmp("format", 4, fmt, T_INT);
            fa = frl(xr_frame, 4);
        }
        rcmp("fv_offset", 0x008, xr_frame[3] & 0xfff, T_H4);
        rcmp("fault_addr", addr, fa, T_H8);
    }
    rend();
    kend("bus_error_unmapped_read");
}

/* ------------------------------------------------------------ hot loops */
static ULONG hmix(ULONG h, ULONG v)
{
    return ((h << 5) | (h >> 27)) ^ v;
}

/* per-exception record of the current / first hot_frames iteration, so a
 * divergence can be pinned to one exception */
#define HOTN 32
static ULONG hot_cur[HOTN][7], hot_first[HOTN][7];
static int hot_n;

static ULONG hrec(ULONG h, ULONG ret, ULONG at)
{
    if (hot_n < HOTN) {
        ULONG *r = hot_cur[hot_n++];
        r[0] = ret;
        r[1] = ret ? xr_spbase - xr_ssp : 0;
        r[2] = ret ? xr_frame[0] : 0;
        r[3] = ret ? frl(xr_frame, 1) - at : 0;
        r[4] = ret ? xr_hsr : 0;
        r[5] = ret && !IS000 ? xr_frame[3] : 0;
        r[6] = ret && !IS000 && (xr_frame[3] >> 12) == 2 ? frl(xr_frame, 4) - at : 0;
    }
    h = hmix(h, ret);
    if (!ret)
        return h;
    h = hmix(h, xr_spbase - xr_ssp);
    h = hmix(h, xr_frame[0]);
    h = hmix(h, frl(xr_frame, 1) - at);
    h = hmix(h, xr_hsr);
    if (!IS000) {
        h = hmix(h, xr_frame[3]);
        if ((xr_frame[3] >> 12) == 2)
            h = hmix(h, frl(xr_frame, 4) - at);
    }
    return h;
}

static ULONG hot_frames(ULONG it)
{
    ULONG h = 0x811c9dc5UL, at;
    int n;
    hot_n = 0;
    for (n = 0; n < 16; n++) {
        at = A(xs_trap) + 4UL * n;
        xset(0x2700 | n);
        h = hrec(h, xrun(at), at);
    }
    xset(0x2709);
    xr_in[0] = 0xffff;
    xr_in[1] = 10;
    h = hrec(h, xrun(A(xs_chkw)), A(xs_chkw_at));
    xset(0x2700);
    xr_in[0] = 0x1234;
    h = hrec(h, xrun(A(xs_divuw)), A(xs_divuw_at));
    xset(0x2702);
    h = hrec(h, xrun(A(xs_trapv)), A(xs_trapv_at));
    xset(0x2700);
    h = hrec(h, xrun(A(xs_ill)), A(xs_ill));
    at = A(xs_ill) + 8 * 21;                    /* $a000 */
    h = hrec(h, xrun(at), at);
    if (GE020) {
        for (n = 0; n < 3; n++) {
            at = A(xs_tcc) + (ULONG)(n * 16 + 7) * 8;    /* TRAPEQ */
            xset(0x2704);
            h = hrec(h, xrun(at), at);
        }
        xset(0x2700);
        xr_in[0] = 0x00010000UL;
        xr_in[1] = 0xffff;
        h = hrec(h, xrun(A(xs_chkl)), A(xs_chkl_at));
        xset(0x2700);
        h = hrec(h, xrun(A(xs_divul)), A(xs_divul_at));
    }
    if (it == 0)
        for (n = 0; n < hot_n; n++) {
            int k;
            for (k = 0; k < 7; k++)
                hot_first[n][k] = hot_cur[n][k];
        }
    return h;
}

static ULONG hot_trace(ULONG it)
{
    ULONG h = 0x01000193UL, r;
    int i, j;
    (void)it;
    r = trace_run(A(xs_tr1), 0, 0);
    h = hmix(h, r);
    h = hmix(h, xr_nlog);
    for (i = 0; i < xr_nlog && i < XLOGMAX; i++) {
        h = hmix(h, xr_log[i].vec);
        h = hmix(h, xr_log[i].hsr);
        h = hmix(h, frl(xr_log[i].f, 1) - A(xs_tr1));
        /* SR and format/vector word; the PC and the format $2 address are
         * hashed as offsets (absolute words would make the hash, which a
         * failure reports, depend on the memory layout) */
        for (j = 0; j < 4; j++)
            if (j != 1 && j != 2)
                h = hmix(h, xr_log[i].f[j]);
        if (GE020)
            h = hmix(h, frl(xr_log[i].f, 4) - A(xs_tr1));
    }
    return h;
}

static void hot_case(const char *id, ULONG (*fn)(ULONG), const char *why)
{
    ULONG first = 0, bad = 0;
    long r = amt_hot(fn, amt_hot_reps(), &first, &bad);
    if (r < 0) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_hex("hash", first);
    json_obj_end();
    json_obj("got");
    json_hex("hash", bad);
    json_int("iteration", r);
    json_obj_end();
    json_str("why", why);
    json_obj("ctx");
    json_str("cpu", amt_probe_result.cpu);
    if (fn == hot_frames) {
        int n, k;
        for (n = 0; n < hot_n; n++) {
            for (k = 0; k < 7 && hot_cur[n][k] == hot_first[n][k]; k++)
                ;
            if (k < 7)
                break;
        }
        if (n < hot_n) {
            json_int("exception_index", n);
            json_str("layout", "[vector, frame size, SR, PC offset, handler SR, fv, addr offset]");
            json_arr("first");
            for (k = 0; k < 7; k++)
                json_hex(NULL, hot_first[n][k]);
            json_arr_end();
            json_arr("diverged");
            for (k = 0; k < 7; k++)
                json_hex(NULL, hot_cur[n][k]);
            json_arr_end();
        }
    }
    json_obj_end();
    amt_case_close();
}

/* ------------------------------------------------------------ entry */
void amt_run_exceptions(void)
{
    int v;
    ULONG usp0 = xr_get_usp();
    cpu = AMT_CPU();
    if (cpu <= AMT_CPU_68000) fam = 0;
    else if (cpu == AMT_CPU_68010) fam = 1;
    else if (cpu <= AMT_CPU_68020) fam = 2;
    else if (cpu <= AMT_CPU_68030) fam = 3;
    else if (cpu <= AMT_CPU_68040) fam = 4;
    else fam = 5;
    pool = (UBYTE *)amt_any_pool();
    {
        char msg[48], *p = msg;
        const char *s = "exceptions: family ";
        while (*s)
            *p++ = *s++;
        s = fam_name[fam];
        while (*s)
            *p++ = *s++;
        *p = 0;
        amt_log(msg);
    }

    /* our recording stubs on every vector except the interrupts (24-31),
     * chaining to the runtime catch-all whenever no snippet runs */
    for (v = 2; v < 64; v++) {
        if (v >= 24 && v <= 31)
            continue;
        xr_old[v] = (ULONG)amt_vec_set(v, (amt_vec_fn)xr_stub_table[v - 2]);
    }

    t_trap();
    t_trapv();
    t_trapcc();
    t_chk();
    t_chk2();
    t_div0();
    t_illegal();
    t_fline_fpu();
    t_priv();
    t_mfsr000_value();
    t_aerr();
    t_misaligned();
    t_trace();
    t_nested();
    t_rte();
    t_unimpl060();
    t_berr();
    hot_case("hot_frames", hot_frames,
             "repeating the same TRAP/CHK/DIV0/TRAPV/illegal/line-A (and 020+ TRAPcc/CHK.L/DIVU.L) "
             "exceptions gave a different frame on a later iteration (JIT vs interpreter?)");
    hot_case("hot_trace", hot_trace,
             "repeating the same T1 trace sequence gave different trace frames on a later iteration");
    xr_set_usp(usp0);
}
