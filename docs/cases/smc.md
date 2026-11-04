# smc (module id 5, AMT_PH_BARE)

Self-modifying code, prefetch and caches, per CPU variant. Templates in `disk/modules/smc_asm.s` are copied into the pools, run, patched with data writes and run again. Cache control is in `smc_020.s` (MOVEC CACR/CAAR) and `smc_040.s` (CPUSH/CINV; the data cache is never invalidated without a push, except for CINVL DC on a line the module owns entirely). Every case runs under `amt_hot()` and is wrapped in `amt_try()`, and CACR is restored to the value found after each case.

Observations: `old` (the unpatched code ran), `new`, `mixed` (an immediate assembled from old and new 16-bit halves: the two cached longwords it straddles came one stale, one fresh), `other`, `warmup_wrong` (the unpatched code did not even give the old value), `blit_not_copied`. When the architecture leaves the result undefined, every allowed value passes, and what was seen goes into `ctx.undef_observed` (this may legitimately differ between emulators and JIT on/off). Strict cases allow only one value. "fam" is 000/010, 020/030 or 040/060.

| case | intent | rule (manual) | expected | a failure usually means |
|---|---|---|---|---|
| `selfmod_d0_cold` / `_warm` | a one-word MOVE.W d1,(a0) patches the instruction right after itself (D=0) with MOVEQ #1; warm = routine ran once before | 68000/68010 UM: two-word prefetch, so the next word is already fetched when the writer runs | 000/010: old; 020+: old or new (pipeline) | no 68000 prefetch emulation (the new word runs) |
| `selfmod_d2_cold` / `_warm` | patch 2 bytes after the next instruction | 68000: fetched by the writer's final prefetch, after the write (bus-cycle order, not in the PRM) | old or new on all CPUs | (no failure expected) |
| `selfmod_d4_cold` / `_warm` | patch 4 bytes after | 68000/010: not yet fetched | 000/010: new; 020+: either | prefetch emulated too deep on 68000 |
| `selfmod_d8_cold` / `_warm` | patch 8 bytes after | as d4 | 000/010: new; 020+: either | as d4 |
| `loop_selfmod` | MOVE.W d1,(a0); DBF loop whose one instruction overwrites itself with ADDQ.W #1,d2, 8 passes | 68000: the taken DBF refills the prefetch from memory, so new; 68010 UM loop mode: no opcode fetches while in loop mode, and whether the first DBcc rereads the word is not specified, so old or new | 68000 new; 68010 either; 020+ either (cache) | 68000: stale prefetch after a taken branch |
| `loop_selfmod_icoff` | same with the I-cache disabled | 020/030 UM: no cache, and a taken branch refetches | 68000 new; 68010 either; 020/030 new; 040/060 either (copyback data cache) | I-cache disable not honoured (JIT/cache emulation keeps stale code) |
| `ic_noflush_far` | far routine (another place, entered by JSR): run, patch its immediate, run without maintenance | 020-060 UM: the I-cache is not coherent with data writes | 000/010 new; 020+ old/new/mixed | 000/010: stale code with no cache |
| `ic_flush_far` | as above + 020/030 CACR C (clear I-cache), 040/060 CPUSHA BC (+060 CACR CABC) | 020/030 UM CACR C bit; 040/060 UM CPUSH | new | the flush does not invalidate (cache emulation or JIT translation cache) |
| `ic_off_far` | I-cache disabled (020/030 CACR E=0, 040/060 IE=0; 040/060 push DC after the patch) | with the cache disabled, fetches go to memory | new | stale code with the cache disabled (JIT not invalidating when the cache is off) |
| `ic_clear_entry_far` | 020/030 only: CAAR = each patched longword, CACR CE | 020/030 UM CAAR/CE | new | CE/CAAR ignored or the wrong entry cleared |
| `ic_freeze_cold_far` | 020/030 only: CACR = E\|F\|C, then run (a miss, not filled), patch, run | 020/030 UM CACR F: a frozen cache does not replace on a miss | new | freeze not honoured (the line is filled anyway) |
| `ic_freeze_warm_far` | 020/030 only: run (cached), freeze, patch, run | frozen hits keep serving | old/new/mixed | (exception or garbage only) |
| `cpushl_bc_far` | 040/060 only: patch, CPUSHL BC,(line) | 040/060 UM CPUSH: push the dirty data line, invalidate both caches' line | new (skipped when the OS left the MMU enabled, TC.E=1: CPUSHL/CINVL take a physical address and the module only knows logical ones) | CPUSHL/CPUSHP does not invalidate the instruction-cache line |
| `cpushl_dc_cinvl_ic_far` | 040/060 only: CPUSHL DC,(line); CINVL IC,(line) | 040/060 UM | new (skipped when TC.E=1, as `cpushl_bc_far`) | line push/invalidate wrong |
| `cinva_ic_only_far` | 040/060 only: CINVA IC without pushing the data cache | copyback data may still be only in the data cache | old/new/mixed | (exception or garbage only) |
| `cpusha_dc_cinva_ic_far` | 040/060 only: CPUSHA DC; CINVA IC | 040/060 UM | new | CPUSHA DC or CINVA IC broken |
| `gen_imm_any` | MOVE.L #imm,d0; RTS generated in the any pool; every iteration patches the immediate, flushes (per CPU, as ic_flush_far), calls | the flush makes the new code visible | new each time (1000x) | JIT/emulator runs a stale translation after a flush |
| `gen_imm_chip` | same in chip RAM | same | new | as above, chip RAM path |
| `gen_opcode_any` | opcode cycled ADD/SUB/AND/OR/EOR.L d1,d0, patch + flush + call | same | new | stale translation after an opcode change |
| `gen_many_any` | 64 routines, 16-byte stride, all patched, one flush, all called | a full flush covers every line | new for all | partial invalidation (only some blocks dropped) |
| `gen_bra_any` | BRA.S displacement byte cycled through 3 targets | same; 060 branch cache cleared by CABC | new | stale branch target (block chaining, 060 branch cache) |
| `dma_blit_code_flush` | chip RAM code overwritten by a blitter A->D copy (BLTCON0 $09F0, 4 words), then flush | HRM blitter; DMA is not seen by the CPU caches, so a flush is needed | new (or `blit_not_copied` if the blit itself failed) | JIT/cache not invalidated for DMA-written code even after a flush |
| `dma_blit_code_noflush` | same without a flush | 000/010: no cache | 000/010 new; 020+ old/new/mixed | 000/010: stale code |
| `cacr_restored` | CACR read back equals the value found at module start | MOVEC CACR | equal | CACR write/read inconsistency |
