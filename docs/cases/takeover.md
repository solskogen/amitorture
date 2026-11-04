# takeover (module id 0, AMT_PH_OS)

The runtime can take the machine over from the OS and give it back intact. It runs with exec alive, does its own Forbid()/Disable(), and calls the same `takeover_save()`/`takeover_take()`/`takeover_restore()` the bare phase uses (`disk/rt/takeover.c`). It is the first module on the core disk: if it fails, every later module started from a damaged machine. Source: `disk/modules/takeover.c`.

| case | intent | rule | expected | a failure usually means |
|---|---|---|---|---|
| `takeover_restore` | save, take over and restore leave the interrupt and DMA enables and the exception vectors as they were | HRM: INTENAR/DMACONR read back what INTENA/DMACON set; the vectors 2-63 at the VBR are plain memory | INTENAR & $7FFF and DMACONR & $07FF as saved; 0 vectors changed (`bad_vectors`) | the read-back registers wrong (a SET/CLR write handled wrongly), or the vector table not restored (68010+: VBR read or used wrongly) |
