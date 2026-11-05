# network (module 18, AMT_PH_BARE, disk group "exp")

Network cards, register level. Only boards found in `amt_probe_result.boards` with a known
manufacturer/product are touched:

| board | man/prod | chip | registers | interrupt |
|---|---|---|---|---|
| Commodore A2065 | 514 ($0202) / 112 ($70) | AMD Am7990 LANCE | RDP board+$4000, RAP board+$4002 (word), 32 KB RAM board+$8000 (LANCE address $8000-$FFFF) | INT2 |
| MNT ZZ9000 Z2 / Z3 | 28014 ($6D6E) / 3, 4 | firmware Ethernet | +$80 ETH_TX, +$82 ETH_RX, +$84/$86/$88 MAC, +$04 CONFIG, +$C0 firmware version, RX window +$2000, TX window +$8000 | INT6 (INT2 board option) |

**Expectation sources.** A2065: the AMD Am7990 LANCE data sheet (CSR0 bit semantics, STOP clearing
CSR0/CSR3, CSR1-3 access while stopped, init block and ring descriptor formats, MODE register,
internal loopback, MCNT including the FCS). The A2065 board layout is the one used by Linux a2065.c,
NetBSD if_le and Commodore's a2065.device. ZZ9000: the MNT ZZ9000 firmware register ABI (as used by
ZZ9000Net.device and emulated by Amiberry zz9000.cpp); there is no chip data sheet and no loopback,
so only register read-back and traffic through the host are tested.

**Ownership and lifetime.** Destructive tests require `amt_board_claim()`, not `unsafe`.
Ordinary unclaimed boot-owned boards retain internal/register coverage without transmission
permission. The helper verifies current BoardList membership, the retained ConfigDev's live
identity/type/serial/address/size, no bound driver, and current CDF_CONFIGME, then atomically
reserves it under bare-phase Forbid/Disable exclusion. Stale probe flags are not ownership;
`unsafe` and `net` never bypass driver-owned/unknown-board rejection. Claims remain reserved
through HALT/REBOOT, including use of other functions on the same board. RETURN skips bare.
The OS probe keeps expansion.library open for those terminal runs and captures the actual
list-head sentinel under configuration binding; bare claims make no blocking OS enumeration call.
A2065 rings, RAM, controller state, pending frames and ZZ9000 backlog cannot be reconstructed:
the suite does not promise full driver/device restoration.
Only the first ConfigDev entry of each family is selected (ZZ9000 Z2/Z3 share a family);
additional cards are logged with address/probe index, without duplicate case IDs. An undersized
selected board is skipped as `board_span`.

Nested `amt_try` scopes run phase-aware cleanup even after exceptions/watchdog aborts, before
the runtime restores OS handlers. A2065 is STOPped and CSR0 checked; ZZ9000 Ethernet interrupts
are disabled and its saved MAC restored. Only then are saved CIA masks restored. Cleanup
failure is reported, not called restoration. No pre-existing driver ownership is resumed.
Unconfirmed LANCE STOP or an exception during cleanup calls `amt_abort_fatal`: permanent in-place
halt, not an ordinary caught abort, OS return, buffer release or continuation with another module.
If fatal reporting itself traps, sticky quarantine stops outer bare recovery at SR=$2700 before
restoration or policy hooks.

CSR3 is always BSWP=1 (68000 byte order), ACON=BCON=0. All A2065 loopback cases use MODE LOOP|INTL
(internal loopback: nothing reaches the cable). Frames are 24 bytes (+4 FCS), inside the Am7990
loopback limit of 32 data bytes; destination = PADR = 02:00:5A:A2:06:05 (locally administered),
ethertype $88B5. Ring: 4 RX and 4 TX descriptors (RLEN=TLEN=2), 1536-byte buffers. CIA-A (INT2) and
CIA-B (INT6) are saved/masked/restored with `cia_hw.h` around the interrupt cases; the hooks have a
storm guard. INT2/INT6 are shared: events without attributable A2065/ZZ9000 pending status
make the IRQ verdict untestable, not an alleged card failure. In particular a level-6 event
alone does not prove the A2065 is miswired.

**External loopback through the host.** Amiberry attaches a network backend with `a2065=<name>`
(cfgfile.cpp; also enables the board) and `zz9000_net=<name>` (default `slirp`): `slirp` is
user-mode NAT (guest 10.0.2.15, gateway 10.0.2.2), `slirp_inbound` the same with ports 21-23/80
open, otherwise a TAP/pcap interface name. `a2065_rom_file=:ENABLED` also enables the A2065 with the
first backend (slirp). The `*_ext_arp` cases send one ARP request for 10.0.2.2 and expect the
reply from 10.0.2.2 to 10.0.2.15 (other frames are skipped); with no backend that answers they are
`untestable`. The request is a broadcast, so it is sent only with run flag `net` (AMT_F_NET, which the
runner sets for entries whose network option is `slirp`) or `unsafe`; otherwise `*_ext_arp` (and on the
ZZ9000 `rx_accept`/`rx_irq`) are untestable (review M4). `<board>_board_rejected` (skip): the board
fails amt_board_ok() and is not touched (review H1).

Both ARP reply oracles check bounded length before reading fixed offsets, Ethernet/ARP
type and protocol, hardware/protocol address lengths, reply opcode, destination/target MAC,
matching Ethernet source/ARP sender MAC, and the complete sender/target IP identity.
A2065 requires a clean single-frame descriptor and MCNT 46..1518 including FCS, within its
1536-byte slot. ZZ9000 bounds length to 42..1518 before copying the ARP header.

## A2065 (`a2065_*`)

| case | intent | rule (Am7990 data sheet) | a failure usually means |
|---|---|---|---|
| `stop_csr0` | CSR0 <- STOP reads $0004 | STOP clears all other CSR0 bits | wrong reset/STOP state |
| `rap_readback` | RAP 0-3 read back (bits 1-0) | RAP | |
| `csr1_rw` | STOP: CSR1 <- $9234 reads back | CSR1 IADR 15-0 (bit 0 = 0) | CSR1 not writable while stopped |
| `csr2_rw` | STOP: CSR2 bits 7-0 <- $A5 read back | CSR2 IADR 23-16 | |
| `csr3_rw` | STOP: CSR3 <- $0004, $0000 read back (bits 2-0) | CSR3 BSWP/ACON/BCON | |
| `init_idon` | INIT from STOP: IDON within 200 ms, CSR0 = $0181 (INIT, IDON, INTR; STOP, TXON, RXON 0) | INIT, IDON, INTR = OR of BABL/MISS/MERR/RINT/TINT/IDON regardless of INEA | init block not read, wrong CSR0 summary bits |
| `idon_clear` | CSR0 <- IDON: CSR0 = $0001 | write-1-to-clear; writing 0 to INIT/STRT/STOP has no effect | |
| `strt` | CSR0 <- STRT after INIT: CSR0 = $0033 (TXON, RXON) | STRT, MODE DTX=DRX=0 | |
| `stop_after_strt` | STOP while running: CSR0 = $0004 | STOP | |
| `stop_clears_csr3` | CSR3 = BSWP before INIT, then INIT, STRT, STOP: CSR3 = 0 | STOP clears CSR3 | |
| `init_strt_dtx_drx` | MODE = INTL, LOOP, DTX, DRX; CSR0 <- INIT and STRT together: CSR0 = $0183 (TXON/RXON 0) | INIT+STRT initialise first, then start under the new MODE | MODE from the previous init used for DTX/DRX |
| `irq_idon` | INIT with INEA: exactly one level-2 interrupt, none at level 6, CSR0 in the handler $01C1, $0041 after the IDON|INEA acknowledge | INEA gates the INTR pin; A2065 wires it to INT2 | wrong level, no interrupt, or not acknowledged |
| `irq_masked` | INIT with INEA=0: no interrupt, CSR0 = $0181 | INEA | interrupt not gated by INEA |
| `lb_transmit` | internal loopback: after TDMD, TINT, TMD1 OWN cleared with ERR=0 ($03xx), TMD3 error bits 0 | transmit descriptor handling | loopback not emulated (frame sent to the cable or underflow) |
| `lb_receive` | the frame arrives in RMD0: RINT, RMD1 = $03xx (OWN 0, no error, STP, ENP), MCNT = 28 (24 + FCS) | internal loopback, MCNT includes the FCS | loopback not emulated |
| `lb_data` | received bytes = transmitted bytes | | byte order (BSWP) or data path wrong |
| `lb_fcs` | DTCR=0: the 4 bytes after the data are the CRC-32 (IEEE 802.3) LSB first | transmitter appends the FCS | FCS missing/wrong order |
| `lb_ring_order` | second frame from TMD1 into RMD1 | descriptors used in ring order | ring pointer not advanced |
| `lb_dtcr_good_crc` | DTCR=1, buffer = 24 data + correct FCS: received without error, MCNT 28, all 28 bytes equal | DTCR: FCS from the buffer, checked by the receiver | |
| `lb_dtcr_bad_crc` | DTCR=1 with a wrong FCS byte: RMD1 ERR and CRC set ($4Bxx) | receiver CRC check | CRC check not emulated |
| `lb_miss` | all RMDs owned by the host: CSR0 MISS and ERR, no RINT; TINT still set | MISS | missed frame not reported |
| `miss_clear` | CSR0 <- MISS clears MISS and ERR | write-1-to-clear, ERR summary | |
| `babl` | skip: needs a >1518-byte transmit, not safe on a real network | | |
| `merr` | skip: every LANCE address decodes to the A2065 RAM | | |
| `ext_arp` | MODE 0 (normal), ARP request for 10.0.2.2 (60 bytes, FCS appended): the ARP reply arrives without error, addressed to PADR, sender 10.0.2.2, target 10.0.2.15; untestable if nothing answers in 2 s | host backend (slirp) | receive path or address filter broken |

## ZZ9000 (`zz9000_*`)

The [official firmware](https://source.mnt.re/amiga/zz9000-firmware/-/raw/master/ZZ9000_proto.sdk/ZZ9000OS/src/main.c)
uses `$8C` as an unused/debug register, **not a queue count**. No private `$8C` extension is
required or read. The firmware version is logged (zero/all-ones cannot identify a usable ABI).
RX uses CONFIG Ethernet-pending plus the legacy length/serial window and ETH_RX advance command;
official firmware ignores the command value, while the emulator also accepts the head serial.
An empty official window can retain stale bytes, so length or serial alone is not readiness.
Before transmitting, pending frames are released until three quiet frames are observed (bounded
to one second); an unquiet backlog is untestable. Later unrelated frames are tolerated, never
assumed absent because one request was sent.

| case | intent | rule (ZZ9000 firmware ABI) | a failure usually means |
|---|---|---|---|
| `mac_rw` | MAC registers $84/$86/$88 <- 02:00:5A:22:90:00 read back (restored at the end) | MAC registers are R/W | |
| `ext_arp` | ARP request through the TX window + ETH_TX; the reply appears in the RX window (length, serial, frame) addressed to the set MAC; untestable without a backend | host backend (zz9000_net=slirp) | TX/RX windows or MAC filter broken |
| `rx_accept` | untestable: legacy firmware provides no universal empty/count indication, so exact single-frame release is not independently measurable | observability limitation, not a 1-to-0 queue assertion | |
| `rx_window_advance` | after the reply is released, a changed RX serial proves the window identity changed; unchanged is untestable (empty/stale versus stuck). Does not prove queue emptiness or exactly one frame consumed | ETH_RX/window observation | |
| `rx_irq` | at least one attributable interrupt at level 6 or 2, with CONFIG Ethernet pending; handler masks the Amiga line for foreground consumption but leaves the device source enabled | card interrupt, concurrent arrivals allowed | missing interrupt/pending |
| `irq_ack` | sample pending, write CONFIG $18, sample pending again **while Ethernet remains enabled**; pass requires an observed pending-to-clear transition with no ambiguous reassertion. Disable occurs separately afterwards. Without a measured transition, or with a backlog/new traffic reassertion, untestable | ACK mode (bit 3), Ethernet ACK (bit 4); disable is not ACK evidence | |

`no_board`: skip when no known board is present.

Native/source-model regressions (no emulator):
`cd runner && TMPDIR=/home/solskogen/source/amitorture/work/tmp PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest -v tests.test_guest_board_claim tests.test_guest_expansion tests.test_guest_expansion_fatal`.
