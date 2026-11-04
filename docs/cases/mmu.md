# mmu (module 20): the 68030 PMMU and the 68040/68060 MMU

Sources: `disk/modules/mmu.c` (tables, cases, expectations) and `disk/modules/mmu_asm.s` (run harness, recording stubs, MMU instruction helpers and the snippets; every MMU/cache/MOVEC/MOVES instruction is `dc.w`-encoded). Disk group `cpux`. Any-pool: 58 KB (16 KB tables, 2 KB user stack, 40 KB data pages).

**Which CPUs.** 68030 (with `mmu_model=68030` in Amiberry; without it Amiberry's 030 has no MMU and the probe reports 68EC030), 68040 and 68LC040, 68060 and 68LC060. EC parts, 68020 (+68851) and older: one `module` skip. If TC.E is already set when the module starts (an OS such as AROS has its own tables), the module reports one `module` skip and touches nothing: the OS tables are unknown, so its MMU state could not be put back. The JIT does not support the MMU (Amiberry turns `mmu_model` off with the JIT), so there are no JIT twins: the cases run on the interpreter cores (fast / compatible / cycle-exact MMU tables 31-35).

**How a case runs.** Every case builds fresh translation tables in the pool and runs its accesses inside `mm_run()` with SR=$2700: the harness loads CRP/SRP/TT0/TT1 (030) or URP/SRP/ITT0/1/DTT0/1 (040/060), flushes the ATC, clears/pushes the caches and enables TC; afterwards it writes TC=0, puts the original TT registers back, flushes the ATC and pushes/clears the caches. The original registers are saved at the start and restored and compared at the end (`regs_restored`). Records are emitted only with the MMU off. SFC/DFC (set by the MOVES/PLPA snippets) are saved and put back around every run; on the 060 every CPUSHA is followed by a CINVA, because with CACR.DPI set CPUSH does not invalidate (060UM 5.3); the restore after an abort also pushes the caches and puts CACR back.

- 68030: everything outside the test window is identity mapped with early-termination page descriptors (CI=1); default layout PS=4K, IS=0, TIA=8, TIB=6, TIC=6.
- 68040/68060: identity page descriptors (CM=10, cache inhibited serialized) for chip RAM, CIAs, custom chips, ROM, payload, stack, vector table and pool; everything else is invalid.
- The test window is a free 32 MB logical range ($AE000000 normally). Its pages map onto data pages in the pool, or read-only onto the ROM for 16 KB and 32 KB pages (030). Data pages hold `physical address ^ $5AA5C33C` per longword, so a value read says where it came from (ctx `markers`).
- The exception stubs (vectors 2-23, 32-63) log {vector, handler SR, SSP, 46 frame words}. A "resume" case installs a hook that fixes the descriptor named for the fault address, flushes the ATC (on the 040 it also performs the pending writebacks from the format $7 frame with MOVES) and returns with RTE. Without a hook a fault ends the run with the MMU off.
- A failure's `ctx` has the window, pool, TC, the observed values and every logged exception frame (raw words). Addresses appear only in failing records.

**No walk.** If basic_translate's reads fail, no U bit was set anywhere on the test page's table path and no read reached a test page, the MMU accepted TC.E=1 but never walked the tables. If the probe ran its own table walk and saw U bits set (hello `how` token `mmu030:walk u=N`), this CPU does have a paged MMU, so this is a **fail** of basic_translate (`table_walk_seen` 0, ctx `probe_saw_walk`) and every other case is skipped behind it. Otherwise the probe's MMU decision rested on weaker evidence (TC reads back, a configuration exception, a 68851 PMOVE), an EC part cannot be told from a broken MMU, and the module reports one `module` **untestable** with the probe's MMU `how` tokens in `ctx.probe_how`. This covers Amiberry's 040/060 without `mmu_model`, which store TC but never translate. A 68EC030 that the probe took for a 68030 and that takes the F-line trap on PMOVE CRP/PFLUSHA/PTEST while basic_translate is set up gives one `module` skip instead of `internal_abort`.

**Implementation-dependent ctx values** (ATC retention after a flush that need not drop an entry, cache staleness where the manual allows old or new, the number of 040 writeback slots used, the fault address of a misaligned access, the 060 stacked PC offset) are reported under `undef_*` keys, which the runner keeps out of fingerprints.

**basic_translate gate.** If the first case fails (the MMU does not translate, e.g. a 68040 without `mmu_model`), every other case reports `skip`. Writes through logical pages outside the pool only happen after it passed.

| case | intent | rule (manual) | CPUs | a failure usually means |
|---|---|---|---|---|
| `module` | skip or untestable | skip: no paged MMU (EC part, or an EC030 that traps on PMOVE CRP), or TC.E already set by the OS; untestable: TC.E=1 accepted but no table walk and the probe saw no walk of its own (EC part or broken MMU: cannot tell) | all | - |
| `basic_translate` | two window pages crossed onto two data pages; reads (long, byte) then a write | the page descriptor's physical address is used (030UM 9.1/9.5, 040UM 3.1, 060UM 4.1) | all | the MMU does not translate (no MMU emulation) or tables walked wrongly; `table_walk_seen` 0: no walk at all although the probe saw one |
| `ps_256` .. `ps_32k` | page size 256 B-32 KB: reads at start/middle/end of both pages, a write near the end (32 KB: read-only onto the ROM) | TC.PS (030UM 9.6.1) | 030 | page offset/index split wrong for that PS |
| `ps_4k`, `ps_8k` | same with TC.P = 0/1 | 040UM 3.1.2, 060UM 4.1.2 | 040/060 | 8 KB page index bits wrong |
| `invalid_read_fault` | MOVE.L (A0),D0 from an invalid page, no fix | vector 2; 030 format $A/$B, SSW DF=1 RW=1 size long FC=5, fault address; 040 format $7 SSW ATC=1 RW=1 TM=5, FA; 060 format $4, FA, FSLW PF=1 RW=read TM=5, PC = the instruction (030UM 8.2.1, 040UM 8.4.5, 060UM 8.4.1) | all | frame format/SSW/FSLW/fault address wrong |
| `invalid_read_resume` | MOVE.L (A0)+,D0, the hook validates the page | read rerun (030 DF) / instruction restarted (040/060): D0 = data, A0+4 once, one fault | all | rerun/restart wrong, A0 incremented twice |
| `wp_read_ok` | read of a WP page | no fault (030UM 9.5.3.2, 040UM 3.2.2) | all | WP applied to reads |
| `wp_write_fault` | MOVE.L D1,(A0) to a WP page, no fix | vector 2, memory unchanged; 030 SSW DF=1 RW=0, data output buffer = D1; 040 SSW ATC=1 RW=0, a valid writeback slot with address = FA and data; 060 FSLW WP=1 RW=write, PC = the instruction | all | frame contents; write reaching memory |
| `wp_write_resume` | MOVE.L D1,(A0)+, the hook clears WP | write done exactly once, A0+4 (030 rerun; 040 handler writeback, no retry; 060 restart) | all | write lost or repeated, A0 wrong |
| `wp_rmw_resume` | ADDQ.L #1,(A0) on a WP page, fixed | memory incremented exactly once | all | 040: instruction restarted after a write fault (+2) |
| `super_user_fault` | user-mode read of a supervisor-only page (030 long format S bit) | vector 2, FC/TM=1, stacked SR S=0, 060 FSLW SP=1 | all | S bit ignored or wrong FC |
| `super_user_resume` | as above, the hook clears S | the user read completes, TRAP #0 follows (2 exceptions) | all | resume to user mode broken |
| `super_access_ok` | supervisor read/write of an S page | allowed | all | S applied to supervisor accesses |
| `misaligned_read_resume`, `misaligned_write_resume` | MOVE.L at page end - 2, the second page invalid; the hook validates it (matching the page that overlaps FA..FA+7, since FA may be the operand start) | after RTE the access completes once with the right bytes on both pages; 040 SSW MA=1, 060 FSLW MA=1 (040UM 8.4.5, 060UM 8.4.1) | all | misaligned continuation after a fault |
| `ifetch_fault_resume` | JSR into an invalid page, the hook validates it | 030 SSW DF=0 with FB or FC set; the faulted fetch address is the long frame's stage B address (+$24; stage C = B-2), not the data cycle fault address at +$10, which is valid only for DF=1 (030UM 8.2.1): the hook matches on it (format $B) or on FB/FC alone (format $A, which has no stage B address), and `ifetch_addr_in_page` checks it lies in the invalid page; 040 SSW ATC=1 RW=1 TM=6, FA = target; 060 FSLW IO=1 PF=1 RW=read TM=6, FA = PC = target; then the code runs | all | instruction-fetch faults |
| `used_bit_read` | read through a fresh page | U set, M clear in the page descriptor (030UM 9.5.3.2, 040UM 3.2.2) | all | history bits not written back |
| `modified_bit_write` | write through a fresh page | U and M set | all | M not set by the walk |
| `modified_after_read` | read then write a page (ATC entry M=0) | the write must set M in memory (030UM 9.4.1, 040UM 3.2.3) | all | M only set on a miss |
| `used_bit_tables` | the read's table walk | U set in the level-A/B (030) or root/pointer (040/060) descriptors | all | U only set in page descriptors |
| `atc_stale_flusha` | change a descriptor without a flush, then PFLUSHA | before the flush old or new page allowed; after it the new page | all | PFLUSHA does not flush |
| `atc_pflush_page` | change two descriptors, flush one page (030 PFLUSH #5,#7,(A0); 040/060 PFLUSH (A0), DFC=5) | the flushed page is new, the other old or new | all | page flush ignores the address or the FC |
| `pflushan_global` | G=1 and G=0 pages changed, PFLUSHAN then PFLUSHA | non-global new after PFLUSHAN; global old or new; both new after PFLUSHA (040UM 3.4, 060UM 4.3) | 040/060 | PFLUSHAN / G handling |
| `pflushn_page` | PFLUSHN (A0) of a non-global page | new translation | 040/060 | PFLUSHN ignored |
| `atc_thrash` | 48 (030) / 160 (040/060) pages on a few physical pages; sequential reads, writes, scattered reads in one run; 3 rounds (1 in quick mode) | every access uses its own translation under ATC replacement (030 22 entries, 040/060 64) | all | ATC replacement mixes up entries |
| `nested_fault` | the fault handler itself reads an invalid page | both faults taken and fixed, nested frame with S=1, both reads correct | all | fault-in-handler state corruption |
| `indirect` | page-level indirect descriptor (030 DT=2, 040/060 PDT=10) | translation through it; U set in the target; the indirect descriptor unchanged (030UM 9.5.1.4, 040UM 3.2.2) | all | indirect not followed, or bits written into it |
| `sre` | TC.SRE: CRP and SRP trees map the page differently | supervisor uses SRP, MOVES FC 1 uses CRP (030UM 9.6.1) | 030 | SRE ignored |
| `fcl` | TC.FCL: FC table in front of level A | FC 1 and FC 5 use different trees (030UM 9.5.2.1) | 030 | FC lookup ignored |
| `urp_srp` | URP and SRP trees differ | user (MOVES FC 1) via URP, supervisor via SRP (040UM 3.1.1) | 040/060 | wrong root pointer chosen |
| `tt0_fc`, `tt1_fc` | TTn base = pool block, FC 1 only, RWM | FC 1 identity, FC 5 via tables (030UM 9.7) | 030 | TT FC compare |
| `tt_fc_mask` | FC base 1, FC mask 4 | FC 1 and 5 both transparent | 030 | FC mask sense |
| `tt_rw_read`, `tt_rw_write` | RWM=0 with RW=1 / RW=0 | only reads / only writes transparent | 030 | RW/RWM decoding |
| `tt_rmw` | TAS through TT0 (FC 5, RW=read) with RWM=0 and RWM=1 | RWM=0: the locked read-modify-write is not transparent (goes through the tables); RWM=1: transparent (030UM 9.7). Skipped when the pool is in chip RAM | 030 | RMW cycles matched like plain reads |
| `tt_addr_mask`, `tt_addr_nomatch`, `tt_disabled` | address mask, mismatched base, E=0 | match only when base matches under the mask and E=1 | 030 | address mask sense |
| `tt_ifetch`, `tt_ifetch_nomatch` | TT with FC 6 / FC 5, JSR into a remapped page | FC 6 matches the fetch (identity code), FC 5 does not | 030 | TT not applied to instruction fetches |
| `ptest0_tt` | PTEST #0 on a TT address | MMUSR T=1 (030UM 9.6.3) | 030 | |
| `dtt0_user`, `dtt1_user`, `dtt_super`, `dtt_any` | DTTn S field 00/01/1x | user / supervisor / both transparent (040UM 3.1.3, 060UM 4.1.3) | 040/060 | S field decoding |
| `dtt_addr_mask`, `dtt_addr_nomatch` | address mask | as for the 030 | 040/060 | |
| `dtt_wp_fault` | DTT W=1 (user), MOVES write FC 1 | access fault, FA, nothing written; 040 SSW RW=0 TM=1; 060 FSLW WP+TTR | 040/060 | TT write protect ignored |
| `itt0_fetch`, `itt1_fetch`, `dtt_not_ifetch` | ITTn / DTT0 and a JSR into a remapped page | ITT matches fetches, DTT does not | 040/060 | ITT/DTT mixed up |
| `ptest040_tt` | PTESTR on a DTT address | MMUSR T=1 | 040 | |
| `plpa_tt` | PLPAR on a DTT address | address unchanged (060UM 4.4) | 060 | |
| `ci_coherency` | 030 cacheable and CI aliases of one page, data cache on | stale cached read allowed until CACR CD, then new; WT write seen at once (030UM 6.1) | 030 | data cache ignores CI / not cleared by CD |
| `cm_copyback_push` | CM=01 alias written, CM=10 alias read | old or new before CPUSHA DC, new after (040UM 4.3, 060UM 5.3) | 040/060 | push does not write back |
| `cm_writethrough` | CM=00 write | the CI alias reads it after a NOP | 040/060 | write-through treated as copyback |
| `cm_stale_cinv` | CI write, cacheable alias read | old or new, after CINVL DC new | 040/060 | CINVL ineffective |
| `early_term_a` | level-A early termination (16 MB → physical 0) | window+ROM offset reads the ROM; PTEST N=1 (030UM 9.5.3.3) | 030 | early termination / remaining bits |
| `early_term_b` | level-B early termination (256 KB → ROM) | offsets pass through; PTEST N=2 | 030 | |
| `levels_2`, `levels_4` | 2 levels (PS 32K, TIA 9, TIB 8; read-only on ROM) and 4 levels (8/4/4/4) | translation; PTEST N = levels (030UM 9.5, 9.6.1) | 030 | TI field handling |
| `levels_is` | IS=4 | window and window & $0FFFFFFF read the same page (030UM 9.6.1); skipped if needed memory is above 256 MB | 030 | IS ignored |
| `limit_upper`, `limit_lower` | long table descriptor limits on the level-B index | out-of-bounds index faults, PTEST L and I set; in-bounds translates (030UM 9.5.1.2) | 030 | limit sense / not checked |
| `limit_root` | CRP upper limit = window index | next level-A index faults (030UM 9.5.1.1) | 030 | root pointer limit |
| `limit_early_term` | long early-termination descriptor with LIMIT 31 | B index 0 translates, B index 62 faults (030UM 9.5.3.3) | 030 | ET limit not checked |
| `pload_ptest0` | PFLUSHA, PTEST #0, PLOADR, PTEST #0 | I=1 before, I=0 after; U set by PLOAD (030UM PLOAD/PTEST) | 030 | PLOAD/PTEST #0 |
| `ptest0_offset` | PTEST #0 of an address inside a loaded page | I=0: the ATC is searched by page | 030 | ATC search compares the page offset |
| `pload_other_fc` | MOVES read with FC 1 (ATC entry), then PLOADR #5 of the same page | the FC 1 entry stays (PTEST #0 FC 1: I=0); entries are tagged by FC (030UM 9.4, PLOAD) | 030 | PLOAD flushes the page for every FC |
| `pload_w_m` | PLOADW | U and M set | 030 | |
| `pflush_fc`, `pflush_fc_mask`, `pflush_fc_ea`, `pflusha_ptest` | ATC entries loaded by reads (plain = FC 5, MOVES = FC 1), then PFLUSH FC,#7 / #4,#4 / FC,#7,(A0) / PFLUSHA, observed with PTEST #0 | only the matching entries flushed (mask bit 1 = compared) | 030 | PFLUSH mask/FC/address matching |
| `ptestr_valid` | PTESTR #7,A1 of a valid page | MMUSR 0 flags, N=3, A1 = page descriptor address, U unchanged, ATC not loaded | 030 | PTEST side effects |
| `ptestr_invalid` | PTESTR #7 of an invalid page | I=1, N=3, A1 = that descriptor | 030 | |
| `ptestw_wp` | WP in the page or in a table descriptor | W=1 both, 0 without | 030 | WP accumulation |
| `ptestr_super_user` | S page, PTEST FC 1 / FC 5 | S=1 / S=0 | 030 | |
| `ptest_modified` | M=1 page | MMUSR M=1 | 030 | |
| `ptest_level1` | PTESTR #1 | N=1, A1 = level-A descriptor | 030 | level limit |
| `config_exc_tc` | TC with E=1 and fields not adding to 32 (TT0 transparent for everything as a safety net) | vector 56 (030UM 9.6.1) | 030 | no configuration exception |
| `config_exc_rp` | CRP with DT=0, MMU off | vector 56 (030UM 9.6.2) | 030 | |
| `ptest040_valid`, `ptest040_wp`, `ptest040_invalid`, `ptest040_super`, `ptest040_modified` | PTESTR/W (A0) | MMUSR PA, G, U1/U0, S, CM, M, W, T, R (040UM 3.5) | 040 | MMUSR layout |
| `plpar_valid`, `plpaw_valid` | PLPAR/PLPAW | A0 = physical (060UM 4.4) | 060 | |
| `plpar_invalid_fault`, `plpaw_wp_fault` | PLPA of an invalid / WP page | access error format $4, FA, FSLW PF / WP, PC = PLPA | 060 | |
| `regs_restored` | after all cases | TC, TT/DTT/ITT, CRP/SRP (030, when the original was valid), URP/SRP, CACR as found | all | module bug (or a register that does not read back) |
| `internal_abort` | only on failure | an exception outside a test window aborted the run | all | module bug or emulator crash |
