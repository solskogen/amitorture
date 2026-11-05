# disk/probe: machine probe

`amt_probe()` (probe.c) fills `struct amt_probe` (probe.h). `amt_probe_json()`
serialises it as the CONTRACT.md section 8 object. Its output is byte-identical
to the `hello.probe` object that `rt/main.c` builds (checked in Amiberry).
The probe runs with exec alive, before the takeover.

The ordinary `amt_probe()` API closes expansion.library before returning and has
no runtime-parameter dependency. The disk runtime alone opts into
`amt_probe_with_claims(p, retain_boards)` for HALT/REBOOT: it keeps a library
reference and live BoardList provenance for authoritative bare-phase claims.
RETURN passes zero, so the reference is already closed before module allocation
or error paths. Its final OS return also calls the idempotent
`amt_probe_release_boards()`; HALT/REBOOT never release claim provenance.
An independent caller abandoning retained provenance must release it with the OS
alive, before any destructive claims or reuse of the probe record. A claimed
terminal run instead keeps its reservation until HALT/REBOOT and cannot return.
Private provenance is not serialised.

Files:

| File | What it holds |
|---|---|
| `probe.h` | The struct (the fields rt/main.c reads, plus `cpu_id`, `cpu_family`, `fpu_id`, `fpu_dis`, `pcr`, `traps` and the `how` string pool), the `AMT_CPU_*`/`AMT_FPU_*` ids, and the API. |
| `probe.c` | Decision logic, the 68020 address-alias test, chipset/exec/memory/expansion info, and JSON. Built with `-m68000`. |
| `probe_asm.s` | `amt_probe_sv()`, which runs one test in supervisor mode with our own vectors, plus the tests themselves. It sets `machine 68060`, `fpu 1` and `opt o-` itself, and the CPU-specific instructions are `dc.w`-encoded, so one object holds everything. |
| `Makefile.probe` | A compile-only check: `make -f Makefile.probe` (or `dis` for a disassembly). |

Isolated cross-check from the project root:
`make -C disk/probe -f Makefile.probe B=../build-audit-expansion-fixes/probe-only -j12`.
This is a compile-only target, not a standalone executable; the probe objects
provide their own `amt_*` functions and assembly helpers. Embedding still supplies
the normal Exec `SysBase` (and target compiler arithmetic support), not disk
runtime globals such as `amt_par`.
For a freestanding 68000 link, `rt/divmod.s` supplies the division helpers this
toolchain normally gets from libnix; libgcc alone does not provide them. Linking
the probe objects with this arithmetic-only shim leaves just `SysBase` imported,
with no unresolved `amt_*` dependency.

## How a test runs

`amt_probe_sv(id, io, vbr, 1)` calls exec `Supervisor()`. The caller holds
`Disable()`, and the routine also raises the interrupt mask to 7. It saves the
vectors 2-15 and 48-61 at `vbr` and replaces them with stubs. Each stub stores
its vector number and the stacked PC. Then the handler resets SP to its value
before the test, which drops the frame on every CPU, and continues after the
test. The vectors are put back and the routine returns with RTE. The return
value is 0 if the test completed, or the vector number if it trapped.

- Illegal instruction (4) and line-F (11) are the expected "not there"
  results.
- Any other vector (bus error 2, address error 3, privilege 8, protocol
  violation 13, format error 14, FPU 48-54, MMU 55-57, 060 unimplemented
  60/61) is caught too. It is recorded as `trap:<test> v=<n> pc=<pc>` in
  `how`, counted in `traps`, and treated as "not there".
- The first test (MOVEC VBR) installs the stubs at address 0. On a 68000 the
  VBR is fixed at 0, so the MOVEC traps through them. On a 68010+ it can't
  trap. Every later test installs the stubs at the VBR it read.

Registers that are written are restored to the value read just before:
CACR, PCR, the 040/060 TC/URP, and the 030 TC/TT0/TT1. Most writes are test bits
that enable nothing (no cache enables); the 030 walk test briefly enables an identity map and disables
transparent translation, then restores it. A saved CRP with DT=0 is not restored and is explicitly reported.
The FPU state is restored with
`FSAVE`/`FRESTORE`.

## Decision table

The `how` strings are listed in the order they appear. `attn:xxxx`
(`SysBase->AttnFlags`) always comes first. It is recorded but never decides.

| Variant | Deciding tests | Expected `how` strings | PRM reference |
|---|---|---|---|
| 68000 | MOVEC VBR,D0 traps (illegal). Cross-check: MOVE CCR,D0 traps; the index scale is ignored. | `movec-vbr:illegal`, `move-ccr:illegal`, `scale:x1`, `fpu:linef`, `addr24:000/010` | M68000PM: MOVEC and MOVE from CCR are "68010 and up"; illegal opcodes take vector 4. |
| 68010 | MOVEC VBR ok, MOVEC CACR traps (CACR doesn't exist; MOVEC with an invalid Rc is illegal). Scale ignored. | `movec-vbr:ok vbr=…`, `move-ccr:ok`, `movec-cacr:illegal`, `scale:x1`, `addr24:000/010` | M68000PM MOVEC table of control registers (CACR: MC68020/030/040/060). M68000PM §2 brief extension word (scale: 020+). |
| 68020 / 68EC020 | CACR ok and scale ×4. No PCR, no ITT0. CACR bits 13 (WA) and 9 (FD) written, both read back 0. Then 020 vs EC020: see the alias row. | `movec-cacr:ok cacr=…`, `scale:x4`, `movec-pcr:illegal`, `movec-itt0:illegal`, `cacr-w:2200 rb=0000`, `cacr:020`, `movec-caar:ok`, `pmove-tc:linef` | MC68020UM §6 CACR (only C, CE, F, E; reserved bits read 0). |
| 020 vs EC020 | Memory above 16 MB in the MemList? If not: EC020 is assumed. If so: under Forbid, find a free chunk H ≥ 16 MB whose `H & $FFFFFF` is a free chunk L below 16 MB, AllocAbs both, write different patterns, and see whether L changed. | `addr24:no-highmem assume 24` (→ 68EC020), `alias:no hi=…` (→ 68020), `alias:yes hi=…` (→ 68EC020), `addr24:highmem, no alias pair, assume 32` | MC68EC020 data sheet: A0-A23 only. With a 24-bit bus a write to H lands in L, which we also own, so only owned memory is ever written. |
| 68030 / 68EC030 | CACR bits 13 and 9 read back as set. MMU: see the next row. | `cacr-w:2200 rb=2200`, `cacr:030 wa+fd`, `movec-caar:ok`, `pmove-tc:ok tc=…` | MC68030UM §6.1.3 CACR (WA bit 13, FD bit 9; ED bit 8 is not touched). |
| 030 MMU | PMOVE TC,(a0): line-F means no MMU. If TC.E is already 1, there is an MMU. Otherwise, if TT0/TT1 can be read, the MMU **table-walk test** runs: active TT entries are temporarily disabled and restored by the assembly cleanup. CRP = `$80000002`/table (L/U=1, DT=2), and the table is 16 identity early-termination page descriptors (DT=1, 256 MB each). TC = `$80F04445` is loaded for 3 instructions and then the original TC is loaded again. If the MMU walked the table, it set the U bit (bit 3) in the descriptors it used. Only a TT-register or walk-test trap falls back to PMOVE TC write/read-back (a configuration exception instead proves MMU presence). | `mmu030:walk u=N` (→ 68030), `mmu030:no-walk u=0` (→ 68EC030), `pmove-tc:linef` (→ EC030), `tc030:e=1`, `tt030:active tt0=…` + `tt030:disabled for the walk test`, `tc030:rw` / `tc030:ro`, `mmu030:config-exception` (vector 56, → MMU present), `crp:dt=0 not restored` | MC68030UM §9 (root pointer, TC, early-termination descriptors, U bit "set by the processor during table search"). |
| 68040 family | No PCR, MOVEC ITT0/DTT0 ok. CACR bits 13 and 9 read back 0. CAAR traps (020/030 only). | `movec-pcr:illegal`, `movec-itt0:ok`, `cacr-w:2200 rb=0000`, `movec-caar:illegal` | MC68040UM §3 / M68000PM MOVEC table (ITT0-DTT1 at $004-$007, URP/SRP $806/$807; CAAR not on the 040). |
| 68040 / LC040 / EC040 | FPU (FMOVE.L FPCR,D0 works) → 68040. Without an FPU: MMU → 68LC040, no MMU → 68EC040. MMU test: if TC.E=1, there is an MMU. Otherwise TC is written with P (bit 14) toggled and E kept 0, then read back. A stored P means an MMU. URP read-back is recorded as a cross-check only. | `fsave:41000000` / `fpu:linef`, `tc:p-rw urp:rw` (MMU), `tc:p-ro …` (no MMU), `tc:e=1 tc=8000`, `cpu:040 fpu without mmu` (a contradiction, reported as 68040, mmu 0) | MC68040UM: the LC040/EC040 take line-F for FPU opcodes; the EC040 has no MMU, so its TC is not implemented. |
| 68060 / LC060 / EC060 | MOVEC PCR ($808) works only on the 060. PCR bits 31-16 = $0430 (68060) or $0431 (EC/LC). `cpu_rev` = PCR bits 15-8. If PCR.DFP (bit 1) is set, it is cleared for the FPU test and then restored (`fpu_dis`=1). No FPU: MMU (TC P bit, as on the 040) → 68LC060, else 68EC060. CACR bit 13 (FIC) reads back as set. | `pcr:ok id=0430 rev=6 dfp=1`, `cacr-w:2200 rb=2000`, `fsave:00006000`, `tc:p-rw urp:rw`; EC/LC: `pcr:ok id=0431 …`, `pcr:dfp stuck`, `fpu:linef`, `tc:p-ro` (EC) | MC68060UM §5 / Appendix (PCR: ID $0430/$0431, revision, DFP); §5.3 CACR (FIC bit 13). |
| FPU 68881 / 68882 | FMOVE.L FPCR,D0 works, then FSAVE (original state), one FPU instruction, FSAVE (idle frame), FRESTORE (original). The idle frame's size byte is $18 for the 68881 and $38 for the 68882 (version byte $1F). | `fsave:1f180000`, `fsave:0x18 68881`; `fsave:1f380000`, `fsave:0x38 68882` | MC68881/882UM §6 (FSAVE idle frame: 881 $18, 882 $38 bytes). |
| FPU 68040 / 68060 | As above. The type follows from the CPU family. The frame is recorded: 040 idle `$41000000`, 060 `$xxxx6000` (format byte $60 = idle). | `fsave:41000000`, `fsave:00006000` | MC68040UM / MC68060UM FSAVE frame formats. |
| no FPU | FMOVE.L FPCR,D0 → line-F. | `fpu:linef` | |

Chipset and other fields:

| Field | Method | `how` |
|---|---|---|
| `agnus_id` | VPOSR ($DFF004) bits 14-8. Alice = `(id & $2E) == $22` ($22/$23/$32/$33). ECS = bit 5 ($20/$30). NTSC = bit 4. | `agnus:23 alice pal` |
| `denise_id` | DENISEID ($DFF07C) read 9 times. If the reads aren't all equal it's OCS and `ffff` is reported. Low byte $F8 = Lisa (AGA), $FC = ECS Denise, anything else = OCS. | `denise:00f8 lisa`, `denise:ffff ocs` |
| `chipset` | Lisa or Alice → AGA. ECS Agnus or ECS Denise → ECS. Otherwise OCS. `ecs_agnus`/`ecs_denise` are also 1 for Alice/Lisa. | |
| `video` | The Agnus PAL/NTSC bit. `SysBase->VBlankFrequency` is a cross-check. | `vblank:50` (`… disagrees` on a mismatch) |
| `kick` | `lib_Version` of exec, `.` and `SoftVer`. | |
| `rom_base` | FindResident("exec.library") rounded down to 256 KB. If the word there is $1111, that's a 256 KB ROM. Otherwise round down to 512 KB ($1114). Only ROM addresses are read. | `rom:1114 512k`, `rom:1111 256k`, `rom:nomagic xxxx` |
| `mem` | `SysBase->MemList` under Forbid (up to 12 entries). | |
| `boards` | OpenLibrary("expansion.library", 33), FindConfigDev(prev, -1, -1) loop, CloseLibrary. If it fails, the array is empty. | `expansion:none` |

## Results in Amiberry

The tests ran in Amiberry (KS 3.1 A1200/A4000/A4000T, 3.2 47.115 and the AROS
ROM). They gave the expected variant for 68000, 68010, 68EC020, 68020 (Z3
RAM), 68030 (+MMU), 68EC030, 68040, 68LC040, 68EC040, 68060, 68LC060,
68EC060, 68881, 68882 and the 040/060 FPUs, plus OCS/ECS Agnus/ECS Denise/AGA
and PAL/NTSC. Notes on how Amiberry models these:

- **The 030 with no `mmu_model`.** Amiberry fakes the PMOVE registers but
  never translates, so the probe reports **68EC030**. Use `mmu_model=68030`
  for a 68030 with MMU. `mmu_model=68ec030` also gives 68EC030.
- **The 040/060 with no `mmu_model`.** Amiberry stores the TC bits, so the
  probe reports an MMU (68040/68LC040/68060/68LC060), as a full CPU would
  have. Use `mmu_model=68ec040`/`68ec060` for the EC parts. Amiberry then
  reads TC back as 0 (040) or with E/P stripped (060).
- **68060 FPU.** Amiberry sets PCR.DFP at reset when no 060 accelerator board
  is configured, as real 060 boards do. The probe clears it for the test and
  restores it, then reports `fpu=68060` with `fpu_dis=1`. The runtime must
  clear PCR bit 1 before running FPU code.

## What this can't prove (real hardware)

- EC030: the probe relies on the MMU really walking the table. It doesn't
  rely on PMOVE trapping, because some EC030s are reported not to trap.
- EC040/LC040: the probe assumes the TC P bit isn't stored without an MMU.
  Amiberry models it that way: "68EC040 always returns zero when TC is read".
- 020/EC020 without RAM above 16 MB can't be told apart. The probe says
  68EC020 and gives the reason in `how`.
- A 68851 on a 68020 is reported only as "PMOVE TC works" (mmu=1). No walk
  test is done.
- The 68080 is out of scope and is not detected.
