# Review: CPU-side test modules (2026-11-03)

This is an independent, read-only review of the tests themselves, not the emulator. No code was changed, the emulator was not run, and nothing was committed. Generated tables were regenerated into work/tmp (since removed) and match the committed ones byte for byte:
- `cpu_basic_tab.s` / `cpu_basic.md`: 78 cases, 644 templates, 13620 vectors.
- `fpu_gen.s`: 89 ops, 1672 vectors.

**Scope:**
- cpu_basic (+ `tools/gen_cpu_basic.py`)
- flags
- exceptions
- smc
- fpu (+ `tools/gen_fpu.py`)
- mmu
- memory + memory_os
- dispatch (+ `tools/gencode.py`)
- the matching `docs/cases/*.md`

The manual sections below come from the reviewers' knowledge of the Motorola manuals. Items marked "check" could not be confirmed against a manual table here.

**Totals: 1 high, 4 medium, 36 low.**

The reference models (gen_cpu_basic, gencode, gen_fpu) held up well. No wrong integer expectation was found. The findings are mostly about robustness, real-hardware fidelity and determinism of failure records.

## Top 5

1. **[high] mmu `ifetch_fault_resume` on a real 68030 uses the wrong frame field for an instruction-fetch fault.** It passes on Amiberry and would fail on silicon. See mmu H1.
2. **[medium] cpu_basic has no `amt_try()`.** One unexpected trap aborts the module, and every remaining case (up to 78) disappears from the records. See cpu_basic M1.
3. **[medium] fpu: the 2-ulp tolerance for 6888x transcendentals has no basis in the manual.** `ftan(-1.5)` is ill-conditioned, so this is a false-fail risk on a real 68881/882 or a faithful core. See fpu M1.
4. **[medium] mmu: a completely broken table walk is reported as a module `skip`, not a failure.** See mmu M1.
5. **[medium] exceptions: on 68020+ every address-error case is skipped.** An odd JMP target still raises vector 3 there, so the 020/030/040/060 address-error frames are never tested. See exceptions M1.

## Cross-cutting notes

- **Fingerprint exclusion is narrower than the brief says.** `docs/schema.md` §4.1 excludes only keys named exactly `undef_observed` and `undef_changed_at` (plus the timing keys). Other `undef_*` names are not excluded. Several fixes below say "move to `ctx.undef_*`". They must either use those two names or the runner must be extended to exclude `undef_*`. Today only smc uses the markers, and it uses them correctly.
- **Failure records contain absolute addresses, which are fingerprinted:**
  - cpu_basic: `ctx.m`, background a-registers.
  - exceptions: `insn_addr`, raw frames, `access_addr`, the `hot_trace` hash.
  - fpu: `got.pc`, `ctx.insn`, `an`.
  - mmu: several ctx fields.
  - smc: `got.pc` on abort.
  - dispatch: `ctx.m`, abort `pc`.
  - memory: `window`, `addr`.

  Pass records are clean. These values are stable within one config, but the same bug on two emulators or memory layouts produces different fingerprints. This is low severity everywhere and listed once per module below. Suggested fix: report offsets from the pool or entry, and put absolute values under an excluded key.
- **DIVU.W overflow C on 68020/030 (finding C3 in docs/findings) is correct:**
  - The PRM says "C always cleared".
  - WinUAE's own hardware-derived comment table above `setdivuflags()` (newcpu_common.cpp:917-927) also says "68020: V=1, C=0, Z=0, N=X". Only the code doesn't clear C.
  - So `cpu_basic.div_w` (and dispatch, whose DIVU.W overflow keeps C defined) are not a false-fail risk there.

---------------------------------------------------------------------------
## cpu_basic (cpu_basic.c, cpu_basic_exec.s, gen_cpu_basic.py)

| # | sev | where | problem | rule | fix |
|---|---|---|---|---|---|
| M1 | medium | cpu_basic.c:488-615 (`run_case`); no `amt_try` in the file | Snippets run without `amt_try()`. Any unexpected trap goes to the runtime catch-all and aborts the whole module, for example an emulator trapping on an in-range CHK, a JIT DIV fault, or an illegal instruction from a mis-decoded full-format extension word. All later cases are then missing, and JIT on/off give different case lists instead of one failing case. | CONTRACT §10 (coverage, parity) | Wrap each case (or each `cb_exec`) in `amt_try()`. On abort, report that case as fail with `amt_last_exc` (vector, PC as an offset from the snippet, template text) and continue. Restore SP from `cb_isp`. |
| L1 | low | cpu_basic.c:610 (`ctx.m`); :373 with :154 | Absolute addresses appear in failure records. Background a0-a6 are `M+$700+$10*i` and are printed raw when they differ. | determinism (schema §4.1) | Print them M-relative and drop `m`, or put it under an excluded key. |
| L2 | low | cpu_basic.c:475-500 | The RMW skip is too coarse. The whole `tas` case is skipped when the any-pool is chip RAM, including `tas d0`, which does no bus RMW. Also, `amt_any_pool_is_chip()` is 0 for A500 slow RAM at $C00000, which is also on the Agnus bus (check). The reason given for a chip-only 68000 for cas/cas2 is "chip RAM", but the real reason is "needs 020". Arguably this is `untestable`, not `skip`. | HRM (chip bus RMW) | Gate only the memory forms (a per-template flag). Treat slow RAM like chip RAM. Fix the reason string. |
| L3 | low | docs/cases/cpu_basic.md (`movem_predec_base_*`, `cmpm`); gen_cpu_basic.py:543-553, :756 | The docs claim MOVEM.L/.W, but only .L has a template. They also claim "CMPM A7 byte by 2", but no vector uses A7. | rule 7 | Add the vectors, or trim the docs. |
| L4 | low | gen_cpu_basic.py:1619; quick-mode hot loop | Two weak checks:<br>- `move.l $4.w,d0 / movea.w #4,a2 / sub.l (a2),d0` compares abs.W against (An) on the same cell, so a common-mode error passes.<br>- In quick mode, `amt_hot_reps()`=100 and the hot loop runs vector `i mod n`, so templates past vector 100 are never hot-checked. | false-pass | Compare against an independently known value (for example SysBase from the probe). Stride the hot index (`i*prime mod n`). |

Coverage gaps (not errors): MOVE16, misaligned CAS, CMPM with A7, and the BFFFO register form with offset ≥32 or negative (deliberately excluded) are not tested.

**Checked and correct:**
- **ADD/SUB/ADDX/SUBX/NEG/NEGX:** C/V/X with X-in; Z cleared if non-zero, else unchanged.
- **CMP/CMPA/CMPI/CMPM:** X kept; operand order; same-register form; CMPI d16(PC) only on 020+.
- **ABCD/SBCD/NBCD:** valid BCD inputs only; N and V masked.
- **Shifts:**
  - Register count mod 64.
  - Count 0: C=0, X kept; ROXd: C=X.
  - Counts ≥ size simulated bit by bit.
  - ASL V = MSB changed at any time.
  - ROd leaves X alone.
- **DIVU/DIVS .W/.L/64:** quotient truncates toward zero; remainder takes the dividend's sign. On overflow: V=1, C=0, destination unchanged, N and Z masked.
- **MULU/MULS .W/.L/64:** V for the 32-bit form; N from bit 63 and Z over 64 bits; Dh≠Dl.
- **CHK/CHK2/CMP2:** undefined flags masked; bounds chosen so signed and unsigned readings agree.
- **Bit fields:** offset mod 32 with wrap in Dn; signed offsets in memory; width 0 = 32; N/Z of the field; BFFFO = offset + index.
- **BTST family:** bit number mod 32 for Dn, mod 8 for memory.
- **Other instructions:** MOVEM base register in the list for 000/010 vs 020+; MOVEP; PACK/UNPK byte order; CAS/CAS2; TAS; EXT/EXTB/SWAP; LINK/UNLK; RTD/RTR; the condition table; MOVE to/from SR and CCR.
- **Addressing modes:** 020 brief and full formats (BS/IS, bd/od, pre/post-indexed, PC/ZPC); misaligned accesses only on 020+.
- **CPU gating:** 060 unimplemented instructions excluded, including the EC/LC parts.
- **Runner:** compares all registers, SP, masked CCR and window; hot loop returns 0/k+1; same ids in quick mode; writes only to pool+0..2047, BSS and stack; interrupt-safe.

---------------------------------------------------------------------------
## flags (flags.c, flags_asm.s, flags_010.s)

| # | sev | where | problem | rule | fix |
|---|---|---|---|---|---|
| L1 | low | flags.c:542 (`sh_cnt` stops at 63) | docs/cases/flags.md claims "register count mod 64", but counts ≥64 are never tested. | PRM ASd/LSd/ROd/ROXd: count modulo 64 | Add counts 64 and 65 (which should act like 0 and 1). The model loop then needs `cnt & 63`. |
| L2 | low | flags.c:1006 (`sr_hi = amt_get_sr() & 0xff00`) | The expected SR upper byte is read back from the CPU, so a wrong upper byte agrees with itself. `move_sr_reserved` catches only part of this. | false-pass | Use the contract value $2000 and assert it once. |

**Checked and correct:**
- Add/sub C and V with carry or borrow in.
- ADDX/SUBX/NEGX Z only ever cleared; X chaining across elements (`model_mp`).
- Shift and rotate rules (as in cpu_basic); the condition table, Scc/Bcc/DBcc (counter expiry, upper word kept).
- ORI/ANDI/EORI/MOVE to CCR; MOVE to SR leaves bits 11 and 7-5 at 0 and never sets M.
- MOVE from CCR only on 010+.
- All `x_vs_c` and preserve entries; producer/gap/consumer layout against `model_fg`.
- The hash covers all of `got[]`; IRQ waits are bounded; vectors are restored.

---------------------------------------------------------------------------
## exceptions (exceptions.c, exceptions_asm.s)

| # | sev | where | problem | rule | fix |
|---|---|---|---|---|---|
| M1 | medium (coverage) | exceptions.c:1069ff (`t_aerr`, the `GE020` branch) | On 020+ all six `addr_err_*` cases are skipped as "data may be misaligned". `addr_err_jmp_odd` still raises vector 3 on 020/030/040/060, because instruction fetches must be word aligned. As a result, the 020+ address-error frames ($A/$B on 020/030, $2 on 040, and the 060 format, check) are never tested. | MC68020UM / MC68030UM address error; MC68040UM 8.2; MC68060UM 8.2 | On 020+ run the JMP case alone: vector 3, the per-CPU format, and on 040/060 the odd target as the fault address. Keep the four data cases as skip. |
| L1 | low | exceptions.c:1294-1301 (`b[]`) | `trace_trap_order` on 020/030 also accepts a second ordering (trap frame first, then trace with SR $A700 and PC = next instruction), citing "68020UM 6.1.4". The standard rule (tracing and multiple-exception priority) gives only `e[]`. WinUAE implements `e[]` too (newcpu.cpp ~2589). | 68020UM tracing / multiple exceptions | Re-check the citation. Drop `b[]` if it doesn't hold. |
| L2 | low | exceptions.c:1371 (`bad040`); docs/cases/exceptions.md | 040 RTE format $4 is in neither the valid nor the invalid list, although LC/EC040 generate $4 frames (the F-line test expects them). | MC68040UM 8.x | Test $4 as valid on the 040 family, or document why it is left out. |
| L3 | low | `kend` (`insn_addr`, raw frame); `ae_run` (`a0`, `access_addr`, `fault_addr`); `t_unimpl060` (`a0`/`a1`); `hot_trace` hash (:1612 mixes the absolute format $2 address words) | Absolute addresses in failure records and in a hash. | determinism | Use offsets. Drop `f[4]`/`f[5]` from the hash, since the offset form is hashed already. |
| L4 | low | exceptions_asm.s ILL entry $0E90 + $4AFC; `xset()` sets a0=0 | If an emulator wrongly decodes MOVES on the 68000, this writes to $0. A wrongly decoded LINK.L $4808 on a 68010 moves SP by $4AFC4AFC. Real hardware is safe (the opcodes are illegal there). | safety under a buggy emulator | Point a0 at pool scratch for the `ill_tab` runs. |
| L5 | low | exceptions.c:1457 `t_berr`, exceptions_asm.s:200-215 (unsafe only) | Resumes without RTE and ignores pending 040 format $7 writebacks (WB1S-WB3S). | MC68040UM 8.2.1 | Check or report (or complete) the valid WBnS on the 040. |
| L6 | low | — | BKPT ($4848-$484F) is not tested. With no breakpoint-acknowledge hardware it gives vector 4 on 010+. | 68020UM BKPT | Add `illegal_bkpt` for 010+. |
| L7 | low (check) | exceptions.c:1255-1259; :878-883 | Two expectations not confirmed against the manual tables: 040 T0 tracing NOP as a change of flow ("68040UM 8.2.6"), and the LC/EC040 and LC/EC060 F-line frames (stacked PC = next instruction, faulted PC at +$0C). Both match WinUAE and the existing findings. | MC68040UM/MC68060UM | Confirm against the UM tables. |

**Checked and correct:**
- **Frame sizes:** formats 0/1/2/3/4/7/8/9/A/B.
- **Format choice:** $2 for CHK/CHK2/TRAPcc/TRAPV/DIV0/trace on 020+; format 0 on the 010.
- **Stacked PCs:** next instruction for TRAP/TRAPcc/CHK/DIV0; the opcode itself for illegal, line A/F and privilege violations.
- **Undefined flags masked:** CHK Z/V/C (and N when no trap), CHK2 N/V, DIV0 N/Z/V; DIV0 C=0.
- **68000 group 0 frame:** R/W, I/N and FC; access address; IR; PC window 2-10 bytes (not exact, correctly).
- **68010 format $8:** SSW and fault address.
- **Bus-error fault-address offsets:** $A/$B +$10, $7 +$14, $4 +8.
- **Invalid RTE format sets:** 010, 020/030 and 060 ({0,2,3,4}, 68060UM 8.2.7).
- **Trace:** T1/T0 PCs and SRs; 040/060 not tracing TRAP; ILLEGAL not traced.
- **060 unimplemented integer:** vector 61, format $0, PC = the instruction, state unchanged.
- **Privilege:** MOVE from SR legal in user mode on the 68000.
- **State and safety:** USP restored; vectors only through `amt_vec_set`; writes only to pool, BSS and stack; trace runs bounded; pass records use offsets only.

---------------------------------------------------------------------------
## smc (smc.c, smc_asm.s, smc_020.s, smc_040.s)

| # | sev | where | problem | rule | fix |
|---|---|---|---|---|---|
| L1 | low | smc.c:560-561 (`blit_copy`) | If the first `blit_wait()` times out, the function returns before restoring DMACON (DMAEN/BLTEN stay set until the runtime's per-module reset). The blitter registers it wrote are also not restored, which matters little because they are write-only. | CONTRACT §10 rule 4 | Restore DMACON on every exit path. |
| L2 | low (real hardware) | smc.c:355-356, :597 (`CPUSHL`/`CINVL` with a logical pointer) | CPUSHL/CINVL take a physical address. The module assumes logical = physical. That holds with the MMU off or identity-mapped, but not if the OS left a non-identity MMU setup running. | MC68040UM/MC68060UM CPUSH/CINV | Note the assumption, or skip the line-form cases when TC.E is set and not identity-mapped. |
| L3 | low | smc.c:189-192 | Abort records carry absolute `got.pc`. | determinism | Report the offset from the routine. |

**Checked and correct:**
- **68000/010 prefetch analysis:**
  - D=0 must be OLD: the word after the writer is in IRC.
  - D=2 is bus-order dependent (MOVE.W Dn,(An) is `nw np`), so both are accepted.
  - D=4 and D=8 must be NEW: fetched by the next instruction's prefetch, whatever the order.
  - `loop_selfmod`: the 68000 taken DBF refetches, so d2=7; the 68010 loop mode accepts both.
- **020/030:** I-cache off forces NEW; CI, CAAR/CE (the 030 entry index uses bits 7:4 and 3:2), and freeze-cold all give NEW; frozen-warm and no-flush accept old, new or mixed.
- **040/060:** CPUSHA BC (+ CABC on the 060), CPUSHL BC, CPUSHL DC + CINVL IC, CPUSHA DC + CINVA IC all give NEW; CINVA IC alone accepts stale.
- **Data cache safety:** the data cache is never invalidated without a push, except CINVL DC on a line the module owns.
- **`classify()` "mixed" logic.**
- **Generated-code cases:** encodings ADD/SUB/AND/OR/EOR.L d1,d0 and the BRA.S displacements 2/6/10.
- **DMA case:** BLTCON0 $09F0, BLTSIZE, a bounded wait, DMACON restored on the normal path, the data-side check through CD/CINVL.
- **Fingerprinting:** undefined outcomes reported only under `undef_observed`/`undef_changed_at`, both excluded.
- **CACR:** restored after every case, including aborts, and checked.
- **Quick mode** keeps every id.

---------------------------------------------------------------------------
## fpu (fpu.c, fpu_asm.s, gen_fpu.py)

| # | sev | where | problem | rule | fix |
|---|---|---|---|---|---|
| M1 | medium | gen_fpu.py:1289-1299 (`TOL = 2`); fpu.c:96 (`ulp_ok`) | Every 6888x transcendental must come within 2 extended ulp of the correctly rounded value. The MC68881/882 UM gives no such bound and the docs cite none. `ftan(-1.5)` is ill-conditioned: r = π/2-1.5 ≈ 0.0708 and \|d cot/dr\| ≈ 200, so a 67-bit-accurate π/2 alone costs about 1.5 ulp. This is a false-fail risk on a real 68881/882, an FPGA or a faithful core. | MC68881/882 UM (accuracy is not specified as ≤2 ulp) | Cite a manual figure, or scale the tolerance by the argument's condition number, or drop `ftan(-1.5)`. Put the measured ulp distance in ctx. |
| L1 | low (check) | gen_fpu.py `int_out_group` (`res_dc=(0xFFFFFFFF,0,0)` on overflow/±inf) | The integer result is masked for out-of-range and ±inf sources. The 6888x OPERR default-result table defines it as the largest integer of the source's sign; only NaN sources are loosely defined. | MC68881/882 UM ch. 6 OPERR | Confirm the table, then check $7F/$7FFF/$7FFFFFFF (or the negative extremes) and mask only NaN. |
| L2 | low | gen_fpu.py `relax6888x` (`op_add`, `emit_reg`); docs say "Amiberry models a 6888x quirk" | The sign of FSUB with opposite infinities on the 6888x is relaxed because of Amiberry's softfloat (softfloat.cpp:2366). The manual result is fully defined, so this justifies a mask by emulator behaviour. | CONTRACT §10 rule 1 | Cite hardware evidence (for example a cputest on a real 6888x) or make the check strict. |
| L3 | low (real hardware) | fpu_asm.s:154-156 | FSAVE follows `fmove.l #1,fp0` with no FNOP. A 68882 that is still executing saves a busy frame, which fails `fsave_idle_frame`. The buffer size is safe. | MC68882 FSAVE (concurrent execution) | Put `fnop` before the FSAVE. |
| L4 | low (check) | fpu.c:546 (`run_excs`, 6888x format $0 only) | For `exc_operr_int` (FMOVE.L FP0,D0, an FP-to-integer move), the 6888x may report the exception mid-instruction. The 020/030 would then stack format $9, not $0. | MC68881 UM exception reporting | Check the UM. If needed, accept $9 for this entry and widen the PC rule. |
| L5 | low | `trap_ctx`, `case_frestore_bad`, `report_mem` | Absolute `got.pc`, `ctx.insn`/`next`, `an` in failure records. | determinism | Use offsets from the trap entry. |
| L6 | low | gen_fpu.py FScc predicate stub | Any byte other than $FF is returned as-is, so a wrong FScc result of $01 reads as "taken" and false-passes. | false-pass | Return a distinct "bad" value for anything other than $00/$FF. |

Coverage gaps:
- S/D-precision register results near the single/double exponent limits (all overflow/underflow vectors run at X precision).
- 040/060 out-of-range integer conversions and S/D output of an SNaN.
- `fsave_frestore_roundtrip` cannot tell a no-op FRESTORE from a correct one.

**Checked and correct:**
- **Number formats:** extended, single, double and packed encodings.
- **Rounding:** all four modes; overflow to ±inf or ±max by mode; UNFL masked for exact tiny results (AUNFL = UNFL & INEX2); register results keep the extended exponent range.
- **FPSR:** condition codes (N for -0/-NaN), quotient byte, accrued rules; INEX1 only for packed input.
- **NaN and OPERR:** NaN propagation (dst first, quieted); OPERR default NaN (inf-inf, 0×inf, 0/0, inf/inf, sqrt(-x)); signed zero; DZ.
- **FMOD/FREM:** FMOD truncates, FREM rounds ties to even.
- **Special-value cases:** FSCALE, FGETEXP, FGETMAN, FINT and FINTRZ edges; FCMP and FTST condition codes; FSGLMUL/FSGLDIV inputs exact in single precision.
- **Reference values:** strict π and powers of ten only where they are far from a rounding boundary.
- **Predicates:** all 32, with BSUN/IOP for $10-$1F with NAN set.
- **040/060 traps:** unimplemented lists and vectors 11/55/60 with their frames; the 040 FSAVE after an unimplemented instruction; OVFL/UNFL kept out of the 040/060 tables (nonmaskable traps expected instead, per the FPSP notes).
- **FSAVE/FRESTORE:** frame sizes and versions for 68881/68882/040/060; a null FRESTORE resets the FPU; a bad frame gives vector 14.
- **FMOVEM:** static, predecrement and dynamic lists.
- **Parity and determinism:** `amt_hot` covers every output; no timing values.
- **Safety:** the FPU is reset at the end and the 060 PCR.DFP is restored.

---------------------------------------------------------------------------
## mmu (mmu.c, mmu_asm.s)

| # | sev | where | problem | rule | fix |
|---|---|---|---|---|---|
| H1 | **high** | mmu.c:357-363 (`fa_of`: on the 030 always frame words 8-9 = +$10); :411 (`hook_fix`); :1106-1150 (`t_ifetch`) | For a 68030 instruction-prefetch fault (DF=0, FB/FC set), +$10 (the data cycle fault address) isn't the fetch address. The fetch address is the stage B address (+$24, in the long format $B frame; stage C = B-2). On silicon, +$10 most likely holds the JSR's stack-push address. No fix entry matches, the hook returns 0, and the run aborts with vector 2, so `ifetch_fault_resume` fails `vec`/`d0`. Amiberry writes `mmu_fault_addr` to +$10 for both $A and $B frames (newcpu_common.cpp:1556), so the test passes there but would fail on hardware. | MC68030UM 8.2.1, format $A/$B frames: the data cycle fault address is valid only for DF=1 | In `fa_of()` on the 030: when DF=0 and FB or FC is set, use stage B (format $B words 18-19, or stage B-2 for FC). For a format $A frame, match on FB/FC. Accept a fix for an ifetch fault only when the SSW says FB/FC. |
| M1 | medium (false-pass) | mmu.c:756-773 | When `basic_translate` fails, no U bit is set and no read reached a test page, the module reports one `module` skip. If the probe saw a real walk (030 `mmu030:walk u=N`) or TC.P reads back (040/060), a completely broken MMU in a new emulator or core shows up as a skip. Baseline diffs catch pass→skip, but a first run doesn't. | CONTRACT §10 rule 1 | Skip only when the probe's MMU decision was weak (the TT-active or TC-rw fallbacks). Otherwise fail `basic_translate`. Use `untestable` with the probe `how` tokens rather than `skip`. |
| L1 | low | mmu.c:1259 `stale_kept`, :1311 `global_kept_by_pflushan`, :1860/:1927 `stale_seen`, :1885 `old_seen_before_push`, :984/:1000/:1100 `writebacks_done`, :1087 misaligned `fault_addr`, :652 `stacked_pc_off` | These implementation-dependent values (ATC retention, cache staleness, number of writeback slots, misaligned FA) are emitted in fingerprinted ctx on failure. The same failure on two cores would show up as a fingerprint mismatch. | determinism | Use the excluded marker keys (see the cross-cutting notes). |
| L2 | low (safety) | mmu.c:2826-2829; mmu_asm.s:247 (`mm_restore_orig`) | After an `amt_try` abort with the MMU on (via an unstubbed vector, 24-31), `mm_disable` is skipped. `mm_restore_orig` clears TC and restores the TT/root pointers, but not CACR (in `mm_orig`), and it doesn't push the caches on 040/060. | rule 4 | Restore CACR and do `CPUSHA BC` there too. |
| L3 | low (safety) | mmu_asm.s:545, :552, :558 | `sn_moves_wr`/`sn_plpar`/`sn_plpaw` set DFC and never put it back. | rule 4 | Save and restore SFC/DFC, or check them in `regs_restored`. |
| L4 | low (real hardware) | mmu_asm.s:138, :179 | The code assumes CPUSHA also invalidates. On the 060 with CACR.DPI (bit 28) set, CPUSH does not invalidate. C code could then read stale descriptor or data lines, giving false fails in `used_bit_*`, `modified_*`, `indirect`, `ptest040_*`. | MC68060UM 5.3 CACR DPI | Follow it with `CINVA DC` on the 060, or clear DPI while the module runs. |
| L5 | low | probe interaction | If the probe reports an EC030 as a 68030 (TT active), PFLUSHA, PMOVE CRP/SRP and PTEST run before the walk check. They may take a line-F trap, which gives `internal_abort` instead of a clean skip. | 68EC030UM | Catch vector 11 during `basic_translate` and turn it into the EC skip. |

**Checked and correct:**
- **Encodings:** every 030 PMOVE/PFLUSH/PLOAD/PTEST `dc.w`; 040/060 MOVEC, PFLUSH variants, PTEST, PLPA, CPUSH/CINV, MOVES.
- **030 formats:** TC fields summing to 32; short and long descriptors; DT semantics including indirect; root-pointer limits; TT registers.
- **030 MMUSR:** bits, the I-with-L rule, N, An, and PTEST level 0.
- **040/060 formats:** TC E/P; 7/7/6 and 7/7/5 index splits; UDT/PDT; page descriptor bits; DTT/ITT; 040 MMUSR.
- **Frame layouts:** $A/$B, $7 (SSW and writebacks, searched in any slot) and $4 (FSLW, BPE masked).
- **Resume rules:** per CPU; RMW and (An)+ applied once.
- **U/M bits:** history-bit rules.
- **Cache staleness:** old/new accepted only where the manuals allow it.
- **Safety:**
  - Tables and pages stay in the pool.
  - Identity maps cover chip RAM, CIA, custom chips, ROM, payload, stack, VBR and pool.
  - The ATC is flushed after every live edit, and caches are pushed on enable and disable.
  - TC is off before the TT registers change; the original registers are restored and checked.
  - Exceptions are capped at 6, and there are no wait loops.

---------------------------------------------------------------------------
## memory / memory_os (memory.c, memory_asm.s, memory_020.s, memory_040.s, memory_os.c)

| # | sev | where | problem | rule | fix |
|---|---|---|---|---|---|
| L1 | low (false-pass on real hardware) | memory.c:107-161 (`rw_window`), :203-264 (`t_cross` window part) | There is no `dc_push()` between write and read in the width and boundary windows. On a real 040/060 with copyback fast RAM, and on cores that emulate the data cache, only the cache is exercised, not the bank's byte lanes. The docs say "where noted", so this is consistent, but it weakens the case's stated intent. | — | Add a push (030 CD, 040/060 CPUSHA DC) before the read-back pass, or a second pass with a push. |
| L2 | low (determinism) | memory_os.c:177-187; memory.c:678-686 | Pass records carry fingerprinted ctx that depends on layout and history:<br>- `<kind>_alloc` holds absolute `lower`/`upper`/`attr` plus `blocks`/`b16m`, which depend on the exec free list at OS phase.<br>- `rom_read` holds `base`/`crc32`/`kick_sum`.<br>The case outcome itself also depends on the free list (`cross16m`: untestable vs pass). All of this is stable for one config and ROM, but a ROM or boot-path difference shows up as a regression. | schema §4.1 | Move the informational values under an excluded key. |
| L3 | low (false-pass, safe) | memory.c:723-729 (`t_rom_write`, 040/060) | With MAPROM or a RAM Kickstart in copyback-cacheable memory, the write allocates a dirty line that CINVL DC discards. Memory is never changed, so the case reports pass instead of skip ("writable"). | — | Acceptable, because it is the safe direction. Optionally push the line instead (then restore) to detect MAPROM. |
| L4 | low (real hardware) | memory.c:391-487, :531 (`move16`/`tas`/`cas` on `z2`/`other`) | PCMCIA memory at $600000 is classified `z2`. Whether Gayle/PCMCIA, Zorro II boards and accelerator bus bridges support indivisible RMW or MOVE16 line bursts (the 040 needs TBI fallback) depends on the board. Only chip and slow RAM are excluded. | HRM; MC68040UM MOVE16 | Classify $600000-$9FFFFF with MEMF from the card as `pcmcia` and skip RMW and MOVE16 there. Consider recording a failure on z2 as "check on hardware". |
| L5 | low (doc) | memory.c:20 (header says "CAS2.W/.L"); :788 (`custom_mirrors`) | Only CAS2.L is tested. `custom_mirrors` always skips (a dead case id). | rule 7 | Fix the header. Drop the case or keep it with a reason. |

**Checked and correct:**
- **CAS:** all 6 vectors (flags of `<ea>-Dc` as CMP, X kept, low byte/word loaded into Dc on mismatch). CAS2.L in both cases (equal → both written; second differs → both loaded, flags from the deciding compare). CAS2 not run on the 060; CAS aligned only.
- **TAS:** N/Z from the old byte, V=C=0, X kept, bit 7 set; skipped on chip and slow RAM.
- **MOVEM:** order, word stores, sign-extending word loads into data registers, and guards.
- **MOVE16:** low 4 address bits ignored and +16 per register, including the misaligned registers.
- **Width and boundary windows:** big-endian image; guards; odd offsets only on 020+.
- **ROM:** CRC with byte vs long reads; 256K/512K size detection.
- **ROM write:** interrupts off, CINVL/CD before the read-back, immediate restore.
- **custom_readback:** DMACONR&$07FF=0, INTENAR=$4020; the long read at $DFF01C.
- **memory_os:** `find_free` keeps blocks inside one free chunk and inside the region; AllocAbs under Forbid; the module writes only its own allocated blocks.

---------------------------------------------------------------------------
## dispatch (dispatch.c, dispatch_asm.s, dispatch_040.s, gencode.py)

| # | sev | where | problem | rule | fix |
|---|---|---|---|---|---|
| L1 | low (determinism) | dispatch.c:479 (`ctx.m`), :611 (abort `pc`) | Absolute addresses in failure records. Registers are correctly reported M-relative. | schema §4.1 | Drop `m`, or move it under an excluded key. Use the PC offset from the block. |
| L2 | low | dispatch.c:285-304 (stubs only for vectors 2-15 and 32-47) | A block that traps through vector 48-63 goes to the runtime catch-all. Examples: an emulator wrongly raising 060 vector 61 (unimplemented integer) on a c040 block, or an FPU/MMU vector. The whole case is then reported with the misleading why "unexpected exception outside the block harness", and the rest of the case's blocks don't run. | — | Stub 48-63 as well (vector 61 especially), so the bisect and the culprit report work for them. |
| L3 | low (coverage/doc) | gencode.py `Gen.mem` (modes ind/d16/d8x/pi/pd/bdx only) | docs/cases/dispatch.md claims "020 addressing modes", but the memory-indirect pre/post-indexed modes ([bd,An,Xn],od) and the PC-relative modes are never generated (cpu_basic covers them in isolation). | — | Add them, or say so in the docs. |
| L4 | low (negligible) | gencode.py `g_adda` (cmpa.l between pointer registers) | Flags are computed from M-relative values. C and V would differ from the absolute comparison only if [M, M+$1000) wrapped or straddled $80000000. | — | Leave as is, or assert in the guest that M+$1000 does not cross $80000000. |

**Checked and correct:**
- **Reference model:**
  - MOVE/MOVEQ/MOVEA (sign extension), ADDQ/SUBQ to An (whole register, no flags), ADDX/SUBX/NEGX (Z sticky), CMPM operand order.
  - EXT/EXTB/SWAP; MULU/MULS .W/.L/64 (Dh≠Dl); DIV .W/.L/64 (overflow: V=1, C=0, N/Z undefined, operands unchanged; Dr≠Dq).
  - ABCD/SBCD/NBCD (valid BCD only; N and V undefined); bit numbers mod 32/8, with immediate counts ≥ size; register shift count mod 64.
  - Bit fields (register wrap, signed memory offset, width 0 = 32; BFFFO register offsets >31 excluded); PACK/UNPK byte order on -(An).
  - MOVE/ANDI/ORI/EORI to CCR (defined-mask bookkeeping); MOVE from CCR only on 020+ classes; TST An/#imm only on 020+.
  - MOVEM -(An) layout; 68000 MOVEM extra-word read kept in mapped memory; the condition-need table (CC_NEED).
  - DBcc semantics; RTD and LINK.L only on 020+; arguments balanced with ADDQ on c000.
- **Re-validation:** every item is simulated again as a whole after generation, so loop and subroutine re-execution is validated; reads below SP are forbidden, so the interrupts the harness allows (IPL 0) can't change visible state; the stack balance is asserted.
- **Gating:** 060-unimplemented instructions kept out of c040; class CPU masks are right.
- **Initial state:** `ds_init` matches `init_state`.
- **Harness:** return address at M+$7FC, SP0 balance; frame-size correction for exception exits.
- **Bisect copy:** M+$800, blocks ≤510 bytes asserted, pool 4096; cache flush per family (020 CI, 030 CI|CD, 040/060 CPUSHA BC + 060 CABC).
- **Hot loop:** compares a hash of the masked state per block; same ids in quick mode; vectors set and restored through `amt_vec_set`.
- **Shared semantics:** f_shift (shared with gen_cpu_basic) was spot-checked here.

---------------------------------------------------------------------------
## Counts per module

| module | high | medium | low |
|---|---|---|---|
| cpu_basic | 0 | 1 | 4 |
| flags | 0 | 0 | 2 |
| exceptions | 0 | 1 | 7 |
| smc | 0 | 0 | 3 |
| fpu | 0 | 1 | 6 |
| mmu | 1 | 1 | 5 |
| memory / memory_os | 0 | 0 | 5 |
| dispatch | 0 | 0 | 4 |
| **total** | **1** | **4** | **36** |

---------------------------------------------------------------------------
## Resolution (2026-11-03)

Every high and medium item is fixed. Low items: fixed where the fix was clearly right, otherwise rejected or deferred with the reason. Case ids are unchanged; new ids: `flags.sr_upper_bare`. Generators changed: gen_cpu_basic.py, gen_fpu.py, gencode.py (doc text only); tables regenerated, `tools/check_generated.sh` reports ok for all six files.

### cpu_basic
- **M1 fixed.** The verification pass and the hot loop of each case run under `amt_try()`. An unexpected trap fails only that case: `expect.exception = none`, `got.vector`/`abort_code`, ctx with the template text, opcode words, vector index, `pc_off` (stacked PC minus the snippet entry; 010+ PC from the frame), format/vector word and SR, plus the inputs. The module then goes on with the next case. (amt_try restores SP and the callee-saved registers, so cb_isp is not needed.)
- **L1 fixed.** `ctx.m` dropped; background address registers that differ are shown as `aN@m` (offset from M).
- **L2 fixed.** Only the memory forms of tas/cas/cas2 are gated (`TAS Dn` runs everywhere); the any-pool counts as chip bus also when it is A500 slow RAM ($C00000-$DBFFFF); the CPU check comes first, so cas/cas2 on a 68000 say "needs 68020" instead of "chip RAM". Kept as `skip` rather than `untestable`: the memory form is not applicable to that memory, which is what skip means (schema: "not run").
- **L3 fixed** by trimming the docs (MOVEM.L only; no "A7 byte" claim for CMPM). A MOVEM.W -(An) base-register vector cannot be relocated (the stored word is the low half of an absolute address).
- **L4 fixed.** New template in `ea_data`: `movea.l $4.w,a2 / move.l 38(a2),d0 / add.l a2,d0 / not.l d0` must give 0 with CCR = Z only (ExecBase->ChkBase is the complement of SysBase; the ADD never carries, so X=C=0). That is an independent check of abs.W. The hot loop strides through the vectors (`i*7919 mod n`), so every vector of a large case can be hot-checked.

### flags
- **L1 fixed.** Register counts 64 and 65 added (16 counts); the model uses `cnt & 63`.
- **L2 fixed.** Expected SR upper bytes use the contract value $2000; new case `sr_upper_bare` asserts MOVE from SR reads $2000 once.

### exceptions
- **M1 fixed.** On 68020+ `addr_err_jmp_odd` runs: vector 3; 020/030 format $A or $B (frame size by the format seen), 040/060 format $2 with the odd target in the address field, checked as an offset. The target with bit 0 cleared is accepted too: the review marks the 040/060 detail "check", the UM text could not be confirmed here, and these buses have no A0 for an instruction fetch. Amiberry stores the even address; stacked SR and handler SR; PC within the existing loose window. The four data cases stay skipped with a 020+ reason.
- **L1 fixed.** The alternative 020/030 ordering `b[]` is dropped. M68000PRM 6.3.8 / 68020UM 6.1.8: an instruction that traps is traced after its exception processing, before the handler's first instruction, i.e. `e[]` only.
- **L2 fixed.** `rte_format_4` now also runs on the 68LC040/68EC040 (they stack format $4). On the full 68040, which never generates $4 and for which the manual lists $4 neither as valid nor as invalid, it is skipped with that reason and stays out of both lists (documented).
- **L3 partly fixed.** `addr_err_*` inputs/fields (`a0_off`, `access_addr_off`, `fault_addr_off`) and the unimpl060 a0/a1 (`*_pool_off`) are offsets; `hot_trace` no longer hashes the absolute format $2 address words. **Deferred:** `kend`'s `insn_addr` and the raw `frame` array (they are the diagnostic payload; see "Cross-cutting" below).
- **L4 fixed.** a0/a1 point at pool scratch for the `ill_tab` runs; the LINK.L entry's filler is `$0000,$0000` (a wrongly decoded LINK.L a0,#0 no longer moves SP by $4AFC4AFC).
- **L5 fixed.** The mode-0 stub completes the valid WB1-WB3 write-backs of a 68040 format $7 frame (sizes long/byte/word; line skipped) before resuming without RTE (MC68040UM 8.2.1).
- **L6 rejected.** BKPT's outcome depends on the board, not the CPU: the 010+ runs a breakpoint-acknowledge CPU-space cycle; DTACK makes the CPU execute the returned word as the replacement opcode, only BERR gives vector 4 (68010UM/68020UM BKPT). On the 68000-bus Amigas (A500/A2000 with a 68010) the function code is not decoded and the cycle (address $0-$1C) is answered by chip RAM, so a deterministic, safe expectation per variant does not exist.
- **L7 deferred.** Confirming the 040 T0/NOP and LC/EC F-line frame expectations needs the UM tables, which were not available here; unchanged.

### smc
- **L1 fixed.** DMACON is restored when the first `blit_wait()` times out.
- **L2 fixed.** If the OS left the 040/060 MMU on (probe `tc:e=1`), `cpushl_bc_far` and `cpushl_dc_cinvl_ic_far` are skipped with the reason, and the DMA case uses CPUSHA DC (the destination line is clean) instead of CINVL DC on a logical address.
- **L3 fixed.** Abort records give the PC as an offset into the any or chip pool (or "outside the pools"), from the frame on 010+.

### fpu
- **M1 fixed.** The MC68881/882 UM gives no ulp bound for the transcendentals, so the 2-ulp tolerance cannot be justified from the manual. It is now applied only to well-conditioned arguments: the generator asserts a relative condition number |x f'(x)/f(x)| <= 3 for every tolerance vector. The badly conditioned vectors were replaced (fsin 10 -> 11; fcos -1.5 -> -3, 10 -> 9.5; ftan -1.5 -> -0.75; fetox 10 -> 3, -20 -> -2; ftwotox 10 -> 3; ftentox 3 -> 1; fsincos 3 -> -2.4). The remaining 2 ulp is an assumption that is stated in docs/cases/fpu.md, not a manual figure. Failures report `ulp_dist` (and `ulp_dist_res2`).
- **L1 deferred (check).** Storing the largest integer of the source's sign on an OPERR integer conversion needs the 6888x UM OPERR default-result table, which could not be confirmed here; the value stays masked.
- **L2 fixed.** FSUB of opposite infinities is checked strictly on the 6888x (dst). The relaxation came from Amiberry's softfloat `faddsub_swap_inf`, i.e. emulator behaviour (rule 1). If hardware evidence of the quirk exists (WinUAE's flag may be hardware-derived), it must be cited before relaxing again.
- **L3 fixed.** FNOP before both FSAVEs (the idle-frame case and the round trip).
- **L4 deferred (check).** Whether the 6888x reports OPERR of FMOVE.L FP0,D0 mid-instruction (format $9) needs the UM; unchanged.
- **L5 fixed.** Trap records give `pc_off`, `fpiar_off`, `frame_addr_off`, `next_off` relative to the instruction (FMOVEM dynamic: the routine); `frestore_bad_frame` gives `pc_off_from_routine`; `fmovem_predec` reports An as final minus start.
- **L6 fixed.** The FScc stub returns 1 only for exactly $FF in the low byte with the upper bytes kept, 0 for $00, and 2 for anything else.

### mmu
- **H1 fixed.** `fa_of()` on the 030: for an instruction-stream fault (DF=0, FB or FC) in a long format $B frame it returns the stage B address (+$24), minus 2 for FC; DF=1 still uses +$10. Fix entries have an ifetch flag: on the 030 an ifetch fix only matches an FB/FC fault (in a short $A frame, which has no stage B address, on that alone) and a data fix only a data fault. `ifetch_fault_resume` marks its fix as ifetch and checks `ifetch_addr_in_page` for $B frames.
- **M1 fixed.** If basic_translate sees no walk at all but the probe saw a real one (`mmu030:walk u=N`), basic_translate fails (`table_walk_seen`, ctx `probe_saw_walk`) and the rest is skipped behind it. Otherwise `module` is `untestable` (was `skip`) with the probe's MMU `how` tokens. (A TC.E=1 left by the OS already makes the module skip before this point.)
- **L1 fixed.** The implementation-dependent ctx values are `undef_*` keys (the runner excludes any `undef_` key, runner/amitorture/cases.py).
- **L2 fixed.** `mm_restore_orig` pushes the caches first (040/060) and restores CACR (030: with CD|CI).
- **L3 fixed.** `mm_run` saves SFC/DFC and puts them back in `mm_cont`.
- **L4 fixed.** On the 060 every CPUSHA (BC and `p40_cpusha_dc`) is followed by CINVA.
- **L5 fixed.** A vector 11 abort while basic_translate is set up on a "68030" becomes one `module` skip (EC030); the restore then runs under `amt_try()` and the CRP restore is skipped.

### memory / memory_os
- **L1 fixed.** `rw_window` pushes the data cache after every write, so the read-back comes from the bank.
- **L2 partly fixed.** The free-list-dependent block flags of `<kind>_alloc` are `undef_*` keys. Kept: `lower`/`upper`/`attr` (the MemHeader, a property of the machine) and `rom_read`'s `base`/`crc32`/`kick_sum` (fixed by the configured ROM, which is part of the config, and the diagnostic payload of a ROM read error). That `cross16m` can be untestable vs pass by free list is inherent to allocating test blocks.
- **L3 rejected.** As the review says, the outcome is the safe direction (pass, memory never changed); no change.
- **L4 fixed.** RAM at $600000-$9FFFFF that is not inside an autoconfig board (A600/A1200 PCMCIA card memory) skips TAS/CAS/MOVE16 with its own reason. The kind stays `z2` because it is part of the case ids.
- **L5 fixed** (header says CAS2.L). `custom_mirrors` is kept as a skip with its reason: the id is a baseline key.

### dispatch
- **L1 fixed** for `ctx.m` (dropped) and the abort PC (`pc_off_block`, from the frame on 010+). The raw `frame` array is deferred (cross-cutting).
- **L2 fixed.** Stubs for vectors 48-63 (vector 61 included), so such a trap is caught per block and bisected.
- **L3 fixed** (docs): the c020 class says it generates the scaled-index brief and full (bd,An,Xn) modes, and that memory-indirect and PC-relative 020 modes are cpu_basic's.
- **L4 rejected.** For the comparison to differ, [M, M+$1000) would have to straddle $80000000. Pools come from RAM, and no Amiga memory map has RAM spanning $80000000 (Zorro III ends at $7FFFFFFF).

### Cross-cutting
- `undef_*` exclusion: the runner already excludes every `undef_` key at any depth (runner/amitorture/cases.py `is_excluded_key`), which matches schema 4.1. No change needed.
- Absolute addresses in failure records: fixed where a single value could become an offset (listed above). **Deferred:** raw exception-frame arrays and the instruction base addresses (exceptions `insn_addr`/`frame`, dispatch `frame`, mmu frames/window/pool, mmu `internal_abort` pc). They need a runner-side excluded key for layout-dependent values (proposal: an `abs_` prefix, excluded like `undef_`). That is a runner change, outside the CPU module files. Pass records were already clean.
- DIVU.W overflow C on 020/030: no action (the review confirms the test).

### Verification (Amiberry, before vs after, same seed)
Configs: 68000 A500 KS1.3 (three group disks, because the whole set does not fit in 512K+512K); 68EC020 A1200 KS3.1; 68030 A3000 `mmu_model=68030 fpu_model=68882`; 68LC040 A4000 `cachesize=0` plus the JIT twin; 68LC060 A4000. The module counters are in the final report. Verdict changes:
- `cpu_basic.tas` on A1200 (chip-only pool): skip -> pass (`TAS Dn` now runs).
- `exceptions.addr_err_jmp_odd` on 020/030/040/060: skip -> pass (the 040/060 pass only because the even address is accepted, see M1).
- `exceptions.rte_format_4` on 68LC040 A4000 (both cores): skip -> pass.
- `mmu.module` on the 040/060 configs without `mmu_model`: skip -> untestable.
- `flags.sr_upper_bare` (new): pass everywhere.
- No case went from pass to fail. FSUB of opposite infinities, now strict on the 6888x, passes on the A3000 68882 config, so the arith_fsub record is unchanged. The re-chosen transcendental vectors leave the trans_* records failing as before on the A3000, for the existing reason (native host-double FPU).
- JIT parity on the A4000: the interpreter/JIT differences are the same set before and after (cpu_basic div_l, neg_negx; dispatch c020_s2, c040_s1; exceptions hot_frames, trace_t0_flow; smc cpushl_bc_far). The changes add none.
- Seen once, not caused by these changes: `smc.loop_selfmod` on the A1200 failed in the baseline run (observed "other" at iteration 904) and passed in both runs after the changes. That looks like nondeterminism in Amiberry's 020 I-cache-off loop path.
