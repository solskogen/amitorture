# devio (module 11, OS phase)

Block I/O through trackdisk.device (units 0-3) and scsi-style devices (scsi.device, 2nd.scsi.device,
3rd.scsi.device, uaehf.device; units 0-7; every exec device node of that name, so the A4091's
scsi.device next to the A4000 IDE one is tested too). Writes happen ONLY on units signed as
test-owned in their first and last block (CONTRACT.md section 11; media from
runner/amitorture/media.py). Case ids are `<dev>_u<unit>_<test>` where `<dev>` is `td`, `scsi`,
`scsi2`, `scsi3`, `uaehf`, plus `_2`, `_3` for a second/third node of the same name (e.g. `scsi_2`).
Every request has a timeout (60 s floppy, 20 s hard disk) through timer.device, then AbortIO and
5 s more; a request that still has not come back is "hung" and stops all further I/O.
Latencies are only in `timing` fields and `perf` records (`<dev>_u<n>_latency` with min/p50/p90/max
microseconds and the number of timeouts; `<dev>_u<n>_write255` / `read255` throughput).

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| devices | lists the device nodes found (name, version) and all exec devices; always pass | - | (diagnostics) |
| `<dev>_u<n>` | skip: no disk / not test-owned / no memory | - | - |
| `<dev>_u<n>_read0` | only with AMT_F_UNSAFE on unowned units: block 0 reads without error (read only) | exec CMD_READ | device read path broken |
| `<dev>_u<n>_owner` | block 0 is signed, so block total-1 must be readable and signed with the same UUID; both blocks agree on first_free/kind, block size 512, kind 1-4 (1 only on trackdisk), non-empty region, total <= 4 GB (else skip); a hardfile's blocks 1-15 hold no RDSK (plain) or only the test-owned RDSK with no partition or the AMTTEST NOMOUNT PART (RDB): a repartitioned medium is not owned (review M2/M3) | CONTRACT section 11 | the device maps blocks wrongly or reports/limits capacity wrongly; unit is not written |
| `<dev>_u<n>_geometry` | the drive's capacity equals the medium size: SCSI READ CAPACITY(10) via HD_SCSICMD for scsi-style devices (the emulated drive), else TD_GETGEOMETRY, else TD_GETNUMTRACKS x sectors/track; all three values in ctx | SCSI-2 READ CAPACITY (last LBA + 1), trackdisk.h DriveGeometry | wrong capacity/IDENTIFY emulation |
| `<dev>_u<n>_geometry_td` | when READ CAPACITY answered: the driver's TD_GETGEOMETRY dg_TotalSectors equals it too | trackdisk.h DriveGeometry dg_TotalSectors | a driver (ROM) quirk if READ CAPACITY is right (seen: A4091 ROM 40.13 reports one sector less) |
| `<dev>_u<n>_write` | skip if the floppy is write-protected | trackdisk TD_PROTSTATUS | - |
| `<dev>_u<n>_rw<N>_al` | N = 1, 2, 7, 16, 64, 255 blocks: CMD_WRITE a known pattern from a 16-byte-aligned buffer (trackdisk: then CMD_UPDATE + CMD_CLEAR), CMD_READ it back into a cleared buffer; every block CRC-checked, io_Error 0, io_Actual = length, 32 guard bytes on both sides untouched | exec CMD_READ/CMD_WRITE, trackdisk.doc, scsidisk.h | data corruption, short transfer, DMA overrun, lost interrupt (timeout) |
| `<dev>_u<n>_rw<N>_w2` | the same with a word-aligned buffer at 2 mod 4 | word alignment is what the device docs require | longword-only DMA / copy paths |
| `<dev>_u<n>_rw<N>_odd` | skip: odd io_Data is not allowed by the device documentation (scsi_Data "word aligned", byte alignment optional) | devices/scsidisk.h, trackdisk.doc | - |
| `<dev>_u<n>_async4` | four 16-block writes in flight (SendIO x4, completions taken as they are replied), then four reads in flight, all verified | exec SendIO/reply semantics | request queueing, completion interrupt lost or duplicated, data mixed up between requests |

Buffers come from AllocMem with the geometry's dg_BufMemType (MEMF_PUBLIC), or MEMF_CHIP on exec < 36
(trackdisk before V36 and DMA drivers need chip RAM there).

TODO: the DF1 results-disk transport is not part of this module yet.
