# fpu (module 9): case ids

The module runs in the BARE phase with AMT_MF_NEEDS_FPU. With no FPU (68LC040, 68EC040, 68LC060, or an 020/030 without a 6888x) the runtime reports a single `module` skip. On a 68060 whose PCR.DFP bit is set, the module clears the bit for the run and restores it afterwards.

## Sources

- **6888x:** MC68881/MC68882 User's Manual (2nd ed.). Chapters 2-4 cover data formats, rounding and the FPSR/FPCR. Chapter 6 covers exceptions and FSAVE/FRESTORE.
- **68040:** MC68040 User's Manual, section 9 (FPU, unimplemented instructions and data types, FSAVE frames).
- **68060:** MC68060 User's Manual, section 6 (FPU, unimplemented instructions and effective addresses, FSAVE frames).
- **Encodings:** M68000 Family Programmer's Reference Manual (FMOVEM masks, the conditional predicates).

## Where the expected values come from

Expected values are computed on the host by `tools/gen_fpu.py`, using exact integer and Fraction arithmetic (decimal for transcendentals) and never the host's double. The generator writes `disk/modules/fpu_gen.s`.

`arith_fsglmul` truncates both input mantissas to 24 bits before multiplication, then rounds the product to single precision with the extended exponent range (M68000 Family PRM, FSGLMUL). The input truncation is independent of FPCR rounding mode and precision. Vectors with `1 + 2^-24` in either or both operands, including a negative operand, distinguish truncation from multiplying the full mantissas in all rounding modes and FPCR precisions. FSGLDIV retains its separate division rule.

## How a table case runs

Each `conv_*`, `arith_*`, `trans_*`, `fcmp_*` and `ftst_*` case is a table of vectors. Each vector runs these steps:

1. Load FP1 (src) and FP0 (dst) raw with FMOVEM, and set FP2 to +0.
2. Set the FPCR (mode and precision) and clear the FPSR.
3. Execute one instruction.
4. Compare FP0 (or the memory written), FP2, and the FPSR (CC, quotient, EXC, AEXC) with the generated values. Each vector has a mask for bits the manuals leave open.

After that, `amt_hot_checked()` executes the whole table for iterations 0 through `amt_hot_reps()`. Every supported vector is checked with the same independent expectations, FPSR/result masks and ULP tolerance used by the cold pass. Architecturally allowed NaN-sign/FPSR or tolerance variants need not be bit-identical between passes; consistently wrong hot results cannot become a baseline. Failures retain the actual failing vector and output.

## Reading a failure

A failing record gives:

- in `in`/`expect`/`got`: the first bad vector (operation, FPCR, src, dst, expected and actual result and FPSR);
- in `ctx`: the number of bad vectors, and up to 5 more in `ctx.more`.

Extended values are printed as `sign+exp|pad . mant_hi . mant_lo`.

## What the masks leave unchecked

The masks deliberately leave these unchecked:

- whether tininess is detected before or after rounding (exact tiny results don't check UNFL);
- INEX2 together with OVFL;
- N on a compare of two zeros;
- the sign of FABS/FNEG of a NaN;
- the integer stored on an integer-conversion overflow (OPERR is checked);
- the j bit of a NaN converted from S/D;
- the quotient sign when the quotient is 0 because of a ±0 dividend or an infinite divisor;
- INEX2 on log(1)=+0;
- INEX2 for transcendentals within the ulp tolerance.

FSUB of opposite infinities (dst - src = dst, an infinity of dst's sign) is checked strictly on every FPU, the 6888x included: the manual result is fully defined. (It used to be relaxed on the 6888x because Amiberry's softfloat models a quirk, `faddsub_swap_inf`, that returns the source infinity; that was a mask based on emulator behaviour, against CONTRACT section 10 rule 1. If hardware evidence for the quirk turns up, cite it here before relaxing again.)

## 040/060 restrictions

The parameter-block flag `AMT_F_FPUCOMPAT` explicitly declares that the host
emulates hardware-unimplemented FPU operations (`fpu_no_unimplemented=false`).
Only `unimp_*`, `unsupp_*`, `effadd_*`, the 6888x `fline_*` trap cases and
040/060 **disabled** overflow/underflow trap expectations are untestable under
that policy, with an explicit reason. They are not passes; strict FPU-policy
lanes must execute those expectations, including a strict 6888x lane for the
040-only opmodes. All enabled arithmetic exceptions and numeric/status checks
remain active. On a compatibility-mode 060 the predicate families and dynamic
FMOVEM computed results are checked, rather than expecting unimplemented traps.
Standalone media with flags zero retain real-hardware expectations. Policy is
never inferred from whether an instruction happens to trap.

With no FPSP, the 68040/68060 trap on these, so the tables give such vectors to the 6888x only and separate trap cases test the 040/060:

- denormal, unnormal and packed operands;
- results that overflow or underflow, since OVFL/UNFL are nonmaskable without the FPSP;
- out-of-range integer conversions.

| case | intent / rule (manual reference) | a failure usually means |
|---|---|---|
| frestore_null_nan | FRESTORE of a null frame resets the FPU: FPCR=FPSR=FPIAR=0, FP0-FP7=NaN $7FFF FFFFFFFF FFFFFFFF (6888x UM 6.4 FRESTORE; 040 UM 9; 060 UM 6) | the reset loads the wrong NaN. In Amiberry native mode this is the known Inf/NaN conversion bug (no PR #2594) |
| fsave_null_frame | FSAVE after a null FRESTORE: 6888x/040 one long with version $00; 060 12 bytes with format byte (byte 2) $00 | wrong FSAVE frame model |
| fsave_idle_frame | idle frame (an FNOP before the FSAVE makes sure a 68882 is not still executing, which would save a busy frame): 68881 size $18 (28 bytes), 68882 $38 (60 bytes), version nonzero; 040 $41000000 (or $40) in 4 bytes; 060 12 bytes with byte 2 = $60 | wrong frame size or version for the emulated FPU |
| fsave_frestore_roundtrip | FSAVE idle / FRESTORE / FSAVE gives the same header and keeps FPCR, FPSR, FPIAR and FP3 (FP3 = FDIV 1/3 in RM: $3FFD AAAAAAAA AAAAAAAA) | FRESTORE of an idle frame corrupts state, or FDIV is not extended precision |
| frestore_bad_frame | FRESTORE of a frame with an invalid version/format takes a format error, vector 14 (020 UM coprocessor interface; 040 UM 9; 060 UM 6) | wrong vector, or (seen on 020/030) the exception raised inside an FPU instruction corrupts the next fetch |
| ctl_reg_bits | after writing all ones: FPCR $0000FFF0 ($0000FFFF also accepted on the 040), FPSR $0FFFFFF8, FPIAR $FFFFFFFF | wrong register widths or masks |
| conv_in_b / _w / _l | FMOVE.B/W/L to FP0: exact sign extension; .L rounded to S/D precision in all 4 modes, with INEX2 | integer conversion or FPCR precision ignored |
| conv_in_s / _d | FMOVE.S/D in: normals, max, min normal, ±inf, QNaN, SNaN (SNAN set, quieted, j don't care); denormals exact (6888x); 0.1 double rounded to S | special values or rounding of S/D input wrong |
| conv_in_x | FMOVE.X in: ±0, ±inf, NaNs, max, SNaN (quieted, SNAN); rounding to S/D precision; unnormal normalized and denormal kept (6888x) | X input treated as a host double, or Inf/NaN conversion broken |
| conv_in_p | FMOVE.P in (6888x): 1, -12.5, 0.25, 1E10, ±0, +inf; 1E-1 sets INEX1 (within 1 ulp) | packed decoder wrong |
| conv_out_b / _w / _l | FMOVE to integer in RN/RZ/RM/RP (ties to even), INEX2; extremes; OPERR on overflow, inf or NaN (6888x); CC unchanged | integer rounding mode ignored or exception bits wrong |
| conv_out_s / _d | FMOVE.S/D out: rounding ties in all modes, overflow to inf/max per mode (OVFL), underflow to denormal or 0 (UNFL, 6888x), specials, SNaN out (SNAN, quiet NaN stored); CC unchanged | destination rounding or range handling wrong |
| conv_out_x | FMOVE.X out is never rounded to the FPCR precision; specials bit-exact; SNaN out quieted with SNAN | extended stored through a double, or Inf/NaN broken |
| conv_out_p | FMOVE.P out with static k=1/3/5/17 (6888x), all modes for inexact values | packed encoder or k-factor wrong |
| inf_roundtrip | FMOVE.X in of ±inf, ±QNaN and default NaNs, then FMOVE.X out: every bit preserved; CC from the load, EXC cleared by the store | the native-mode Inf/NaN bug (needs PR #2594) |
| special_x_inout | the same with ±0, ±max, small normals, SNaN (quieted), denormals and an unnormal (6888x) | special-value conversion |
| fcmp_classes | FCMP over {±0, ±1, 2, ±inf, NaN}²: Z for equal (including equal infinities), N for dst<src and -inf=-inf, NAN unordered, SNAN on an SNaN; the register is unchanged | broken compare of infinities or NaNs |
| ftst_classes | FTST CC for every class (N Z I NAN) | CC computation |
| arith_fadd / fsub / fmul / fdiv | special operand matrix (±0, ±inf, QNaN, SNaN, 1, -3); NaN propagation (dst first); OPERR default NaN; DZ; exact X/S/D ties; overflow in all modes; deep underflow (6888x); 1/3, 2/3, 1/10 in 4 modes × 3 precisions | rounding, precision control or special-value handling wrong (a host double gives 53-bit results) |
| arith_fsqrt | specials, sqrt(-1) OPERR, -0 → -0, correctly rounded sqrt(2), sqrt(3), sqrt(0.5) in all mode/precision combinations | sqrt rounding |
| arith_fmove | FMOVE.X FPm,FP0 rounds to the FPCR precision in every mode | precision control ignored |
| arith_fabs / arith_fneg | specials; rounding to the FPCR precision | sign handling, or no rounding |
| arith_fint / arith_fintrz | FINT in 4 modes (ties to even), FINTRZ toward zero, 2^70+1, specials (6888x and 060; unimplemented on 040) | integer rounding |
| arith_fgetexp / arith_fgetman | exponent/mantissa extraction, ±0 kept, inf OPERR (6888x) | |
| arith_fscale | dst×2^trunc(src), truncation of a fractional scale, overflow per mode, inf scale OPERR (6888x) | |
| arith_fmod / arith_frem | FMOD (quotient truncated, sign of dst) vs FREM (IEEE, round to nearest even); quotient byte = sign + 7 LSBs (FPSR 23-16); x mod inf = x; inf mod y and x mod 0 OPERR (6888x) | quotient byte or remainder rounding wrong; spurious UNFL |
| arith_fsglmul / arith_fsgldiv | result rounded to a single-precision mantissa with the extended exponent range, whatever the FPCR precision; DZ | |
| arith_fsd_variants | 040/060 FSxxx/FDxxx (ADD SUB MUL DIV MOVE SQRT ABS NEG) round to S/D whatever the FPCR precision | |
| trans_fsin … trans_ftanh, trans_fsincos | 6888x transcendentals within 2 ulp (extended) of the exact value, for well-conditioned arguments only (relative condition number |x f'(x)/f(x)| <= 3, asserted by the generator; badly conditioned ones such as ftan(-1.5) were replaced). The MC68881/882 UM gives no ulp bound, so 2 ulp is an assumption, not a manual figure: an accurate implementation's argument-reduction error is scaled by the condition number, which is why only small ones are used. A failure reports `ulp_dist`; exact special operands (sin ±0, cos 0 = 1, etox(-inf) = +0, log 0 = -inf with DZ, log(-1) OPERR, sin(inf) OPERR, atan(±inf) = ±pi/2) | inaccurate transcendentals (host double: about 2^11 ulp) or wrong special cases |
| trans_fmovecr | FMOVECR ROM constants (6888x): exact powers of ten (no INEX2), other constants within 1 ulp; pi correctly rounded in all 4 modes and S/D | ROM constant table or rounding wrong |
| pred_fbcc / pred_fscc / pred_fdbcc / pred_ftrapcc | all 32 predicates × the 8 CC combinations an operation can produce (FMOVE to FPSR): outcome per the predicate formulas (FScc must write exactly $FF or $00 to the low byte of D0 and keep the upper bytes; anything else reads as 2); BSUN+IOP set exactly for predicates $10-$1F with NAN set (BSUN disabled); FTRAPcc traps through vector 7. On the 060 only FBcc runs (the others are unimp_*_060). | predicate table wrong, BSUN not set, or (020/030) wrong state after a taken FTRAPcc |
| fmovem_static | FMOVEM.X (An),FP0-7 / FP0-7,(An) keeps +0, -inf, QNaN, SNaN, denormal, unnormal, max, -1.5 bit-exact | registers stored as host doubles |
| fmovem_predec | FMOVEM.X FP0-7,-(An): FP0 at the lowest address, An -= 96 | predecrement mask order |
| fmovem_dynamic | dynamic list D0=$A5 store (FP0 FP2 FP5 FP7) and (An)+ load with $5A; on the 060: vector 60 (unimplemented effective address) | dynamic mask decoding |
| unimp_* (040/060) | FSIN FCOS FTAN FASIN FACOS FATAN FSINH FCOSH FTANH FATANH FETOX FETOXM1 FTWOTOX FTENTOX FLOGN FLOGNP1 FLOG2 FLOG10 FSINCOS FGETEXP FGETMAN FSCALE FMOD FREM FMOVECR (and FINT/FINTRZ on the 040) take vector 11. The frame is format $2 on the 040 ($0 or $2 on the 060); the stacked PC is the instruction or the next one. On the 040 FSAVE then gives the unimp frame (version $41/$40, size $30). | the emulator executes the instruction (Amiberry: needs fpu_no_unimplemented=true) or builds the wrong frame |
| unimp_fsin_mem | the same with a memory source operand | |
| unimp_fscc_060 / unimp_fdbcc_060 / unimp_ftrapcc_060 | 060: FScc/FDBcc/FTRAPcc are unimplemented, vector 11 | |
| effadd_fmovem_dynamic_060 / effadd_fmove_imm_x_060 / effadd_fmovem_l_imm_060 | 060: FMOVEM with a dynamic register list (see fmovem_dynamic), an #imm .X source and FMOVEM.L #imm of 2 control registers take vector 60 (unimplemented effective address) | the emulator executes them instead of trapping |
| unsupp_packed_in / unsupp_packed_out / unsupp_denormal_x / unsupp_denormal_s / unsupp_unnormal_x | 040/060: packed operands and denormal/unnormal operands take vector 55. FSAVE gives a non-idle frame on the 040 ($30/$60) and format byte $E0 or $60 on the 060. | the emulator converts the operand instead of trapping |
| fline_fsadd_6888x / fline_fdmul_6888x / fline_fssqrt_6888x | 6888x: the 040-only opmodes are illegal coprocessor commands, so F-line (vector 11, format 0) | the emulator accepts 040 opmodes on a 6888x (Amiberry: fpu_no_unimplemented=false) |
| exc_ovfl / exc_unfl / exc_dz / exc_operr / exc_inex2 / exc_snan / exc_bsun / exc_inex1 / exc_operr_int | with the FPCR enable set, the exception traps through vectors 48-54 (BSUN 48, INEX 49, DZ 50, UNFL 51, OPERR 52, OVFL 53, SNAN 54). Checked: format $0 (6888x deferred/pre-instruction), $9 (6888x move-out `exc_operr_int`), or $0/$3 (040/060, BSUN also $2); PC is the op or the following FNOP; the FPSR EXC bit is set; FPIAR is the op (not checked for BSUN). The format-$9 frame must contain the following FNOP as ScanPC and the operation address at +$08 (MC68881/MC68882 UM 1st ed. §6.1.3, §7.4.2.6, Fig. 7-16); a format-$0 or $3 frame is not accepted for this move-out trap. `EXC_MID_6888X` is explicit exception-table metadata, independent of the configured compatibility policy. exc_inex1 (packed input) and exc_operr_int (FMOVE.L of inf) run on the 6888x only. | exception not raised, wrong vector or frame, or (020/030) state corrupted after the trap |
| exc_ovfl_disabled_4060 / exc_unfl_disabled_4060 | 040/060 with no FPSP: overflow/underflow trap through 53/51 even when disabled (the FPSP builds the default result) | emulator returns the IEEE default without the trap |
