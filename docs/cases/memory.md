# memory (module 21) and memory_os (module 25): memory access paths

`memory_os` (AMT_PH_OS) walks ExecBase->MemList and, for every memory region, allocates 2 KB test blocks
centred on a 64 KB boundary (Amiberry's `mem_banks` granularity), on an odd 32 KB boundary and, if the
region spans one, on a 16 MB boundary. It uses AllocAbs() at addresses it found free in the region's own
free-chunk list (falls back to one plain Allocate() block without boundary when no free 64 KB boundary
exists). `memory` (AMT_PH_BARE) then tests every block. On CLI return the runtime frees all blocks,
using `FreeMem` for `AllocAbs` and the original base, size and owning MemHeader with `Deallocate` for
the aligned fallback. Default halt intentionally retains them with the live mailbox.

Region kinds (from the address map and the expansion board list): `chip` (MEMF_CHIP below $200000),
`slow` ($C00000-$DBFFFF), `z2` (Zorro II / $200000-$9FFFFF), `z3` (inside an autoconfig board at or above
$01000000), `mbram` ($07000000-$07FFFFFF, A3000/A4000 motherboard RAM), `hifast` (non-autoconfig RAM at or
above $08000000, e.g. CPU-board / motherboard high RAM), `other`. A repeated kind gets a suffix (`z32`...).
`rom`: a MemHeader that is not RAM (no MEMF_CHIP/MEMF_FAST, e.g. the ROMs AROS puts in the MemList): `<kind>_alloc` reports `skip` and nothing else runs.
`chip` and `slow` are chip-bus RAM (Agnus-arbitrated): no read-modify-write and no MOVE16 there. A `z2` region at $600000-$9FFFFF that is not inside an autoconfig board (on the A600/A1200: Gayle PCMCIA card memory) also skips TAS/CAS/MOVE16: the card interface does not guarantee an indivisible RMW cycle or a line burst.

All expectations follow from the 680x0 big-endian byte order (M68000 PRM 2.2) and the PRM / MC68040UM
instruction definitions; nothing is taken from an emulator. Odd word/long addresses are used only on the
68020+ (68000/010 address errors are the exceptions module's). On 68030/040/060 the data cache is pushed
between writing and reading back where noted, so that the data really goes through memory.

| case | intent | rule (manual) | a failure usually means |
|---|---|---|---|
| `<kind>_alloc` (memory_os) | a 2 KB test block could be allocated in the region; ctx lists the region and which boundary blocks exist (the block flags are `undef_*` keys: they depend on the exec free list, not on the machine, so they stay out of fingerprints) | (exec MemList) | `untestable`: region full or fragmented |
| `<kind>_rw_width` | byte/word/long writes at every offset of a 16-byte window, each read back with every width at every offset (030/040/060: the data cache is pushed after each write, so the reads come from the bank, not a copyback line); 16 guard bytes each side | PRM 2.2: byte n of a word/long at address a is at a+n, most significant first; a write touches only its bytes | wrong byte lane / endianness or width handling in a memory bank's get/put functions, or a write that spills into neighbouring bytes |
| `<kind>_cross64k` | the rw_width window centred on a 64 KB boundary (word/long accesses straddling it on 020+, long at -2 on all), plus MOVEM.L of 14 registers across it (at -28, -26, and -27 on 020+) | as rw_width; PRM MOVEM order | an access split across two 64 KB banks (Amiberry `mem_banks`) is assembled from the wrong bank or the wrong bytes |
| `<kind>_cross32k` | as cross64k on an odd 32 KB boundary | as above | 32 KB granular memory mapping error |
| `<kind>_cross16m` | as cross64k on a 16 MB boundary (`skip` if the region spans none, `untestable` if none is free) | as above | 24/32-bit address decoding or bank table error at a 16 MB edge |
| `<kind>_patterns` | address, inverse address, walking one, checkerboard and byte-ramp (written as bytes, read as longs) over the 2 KB block, cache pushed in between | memory holds what was written | aliasing, stuck bits, lost writes, cache/memory incoherency |
| `<kind>_movem` | MOVEM.L/.W d0-d7/a0-a5 stores and loads at +0, +2 (and +1, +3 on 020+), 4 guard bytes each side | PRM MOVEM: ascending register order d0-d7/a0-a7 at ascending addresses, word stores the low words, word loads sign-extend into all 32 bits | MOVEM burst mis-ordered, wrong width, misaligned MOVEM broken, or bytes written outside the transfer |
| `<kind>_move16` | 4 x MOVE16 (Ax)+,(Ay)+ aligned, and with registers 4/8 bytes past the line start (68040/060; `skip` on chip-bus RAM and other CPUs) | MC68040UM/MC68060UM MOVE16: moves the 16-byte line, address bits 3-0 ignored, both registers += 16 | MOVE16 line alignment or register update wrong, or line burst corrupts memory |
| `<kind>_tas` | TAS on $00, $80, $7F, $FF with CCR in 0 and $1F (`skip` on chip and slow RAM) | PRM TAS: N,Z from the old byte, V=C=0, X kept, bit 7 set; HRM: no indivisible RMW on the chip bus | TAS flags or the write-back wrong for this memory type |
| `<kind>_cas` | CAS.B/.W/.L success and failure vectors; CAS2.L both-equal and second-differs (not on 68060, where CAS2 is unimplemented) (020+; `skip` on chip-bus RAM) | PRM CAS/CAS2: flags of `<ea> - Dc` (CMP), equal: Du written, else <ea> loaded into Dc (low byte/word only) | CAS compare/update semantics or flags wrong, or the RMW cycle not atomic for this memory type |
| `rom_read` | CRC-32 of the Kickstart ROM read with MOVE.B and with MOVE.L (64 KB in quick mode), and the Kickstart checksum over the whole ROM: base and size from where exec's resident tag is (256 KB at $FC0000 for KS 1.x, else 512 KB at $F80000); `kick_sum` in ctx | the same bytes either way (PRM 2.2); the 32-bit sum of all ROM longwords with end-around carry is $FFFFFFFF (the rule Kickstart's reset-time ROM check uses; holds for every Commodore ROM, 3.2.x and Amiberry's built-in AROS pair) | ROM bank byte/long read paths disagree; with only `kick_sum` wrong: a ROM word read wrong, wrong mapping/mirroring or ROM size, or a patched ROM whose checksum was not fixed |
| `rom_write` | word, long and byte writes of the complement to ROM base+$100 (interrupts off), data cache line invalidated, read back; restored at once if it changed | ROM is read-only; `skip` if it read back the written value (MAPROM / RAM Kickstart) | ROM bank accepts writes, or a write corrupts the ROM to a third value |
| `custom_readback` | DMACONR & $07FF = 0 after the case clears every DMA enable bit (DMACON=$07FF; it then restores the state it was handed, which depends on the status screen), and INTENAR = $4020 after the runtime's INTENA=$C020; a long read at $DFF01C gives INTENAR in the upper word | HRM: DMACONR/INTENAR return the DMACON/INTENA bits; 32-bit accesses to the custom chips are two word accesses | custom register read-back or long-access splitting wrong |
| `custom_mirrors` | always `skip` | the HRM defines no address mirrors of the custom chip registers | - |
| `regions` | only when memory_os did not run (`untestable`) | - | module 25 disabled in the module mask |

Perf records (timing only): `read_<kind>`, `write_<kind>` (MOVE.L over the 2 KB block, 32 passes; 8 in quick
mode), `movem_<kind>` (MOVEM.L copy of 1 KB), `rom_crc2` (both ROM CRC passes). Ticks are colour clocks.
