; amitorture bootblock (CONTRACT.md section 2)
;
; Entry (from the Kickstart strap): A1 = trackdisk IOStdReq, A6 = SysBase.
; Exit:  D0 = 0, A0 = dos.library rt_Init  -> boot DOS
;        D0 = -1                           -> boot failure
;
; Loads the payload header (sector 2), allocates the payload's two segments
; (code; data + BSS), reads the blob through a chip RAM track buffer, checks
; its CRC-32, relocates, clears the BSS, stops the motor, clears the caches
; (exec >= 37) and calls the payload entry.
;
; Only exec LVOs from KS1.2 are used, except CacheClearU (guarded).
; Before V36, trackdisk.device needs chip RAM buffers: the header copy and
; the track buffer are chip RAM, the segments go wherever AllocMem puts them
; (fast or slow RAM first), so a 512K chip + 512K slow A500 can hold the
; payload in two pieces.
;
; Build: vasmm68k_mot -Fbin -m68000 -no-opt -o bootblock.bin bootblock.s
; mkadf.py fills in the checksum at +4.

_LVOFindResident	equ	-96
_LVOAllocMem		equ	-198
_LVOFreeMem		equ	-210
_LVODoIO		equ	-456
_LVOCacheClearU		equ	-636

LIB_VERSION		equ	20
RT_INIT			equ	22

IO_COMMAND		equ	28
IO_ERROR		equ	31
IO_LENGTH		equ	36
IO_DATA			equ	40
IO_OFFSET		equ	44

CMD_READ		equ	2
TD_MOTOR		equ	9

MEMF_CHIP		equ	2

BOUNCE			equ	11*512		; one track

; payload header fields
H_MAGIC			equ	0
H_VERSION		equ	4
H_IMAGE_SIZE		equ	8
H_BSS_SIZE		equ	12
H_RELOC_COUNT		equ	16
H_ENTRY			equ	20
H_BLOB_SECTORS		equ	24
H_BLOB_CRC		equ	28
H_ALLOC_SIZE		equ	32
H_ALLOC_FLAGS		equ	36
H_DATA_SIZE		equ	40
H_DATA_SECTORS		equ	44
H_DATA_ALLOC		equ	48

CUSTOM			equ	$dff000
VHPOSR			equ	$006
COLOR00			equ	$180

	section	boot,code

bb_start:
	dc.b	"DOS",0
	dc.l	0			; checksum, filled in by mkadf.py
	dc.l	880			; rootblock

; ---------------------------------------------------------------------------
bb_code:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	a1,a5			; a5 = boot IOStdReq
	sub.l	a4,a4			; a4 = header buffer (none yet)
	sub.l	a2,a2			; a2 = segment 0 (code)
	sub.l	a3,a3			; a3 = segment 1 (data + BSS)
	moveq	#0,d5			; d5 = track buffer

	; 1. header buffer and track buffer, read sector 2
	move.l	#512,d0
	moveq	#MEMF_CHIP,d1
	jsr	_LVOAllocMem(a6)
	move.l	d0,a4
	tst.l	d0
	beq	.fail
	move.l	#BOUNCE,d0
	moveq	#MEMF_CHIP,d1
	jsr	_LVOAllocMem(a6)
	move.l	d0,d5
	beq	.fail

	move.l	a4,a0
	move.l	#512,d0
	move.l	#1024,d1
	bsr	td_read
	bne	.fail

	cmp.l	#'AMTP',H_MAGIC(a4)
	bne	.fail
	cmp.l	#2,H_VERSION(a4)
	bne	.fail

	; 2. the two segments
	move.l	H_ALLOC_SIZE(a4),d0
	beq	.fail
	move.l	H_ALLOC_FLAGS(a4),d1
	jsr	_LVOAllocMem(a6)
	move.l	d0,a2
	tst.l	d0
	beq	.fail
	move.l	H_DATA_ALLOC(a4),d0
	beq	.fail
	move.l	H_ALLOC_FLAGS(a4),d1
	jsr	_LVOAllocMem(a6)
	move.l	d0,a3
	tst.l	d0
	beq	.fail

	; 3. read the blob: code sectors into segment 0, data + relocs into 1
	move.l	a2,a0
	move.l	H_ALLOC_SIZE(a4),d3
	move.l	#1536,d4
	bsr	bounce_read
	bne	.fail
	move.l	a3,a0
	move.l	H_DATA_SECTORS(a4),d3
	moveq	#9,d0
	lsl.l	d0,d3			; * 512
	bsr	bounce_read		; d4 = offset after the code
	bne	.fail
	move.l	d5,a1
	move.l	#BOUNCE,d0
	jsr	_LVOFreeMem(a6)
	moveq	#0,d5

	; 4. CRC-32 (IEEE, reflected, poly $EDB88320) of the blob bytes:
	;    segment 0 (alloc_size bytes), then data + reloc table
	moveq	#-1,d1
	move.l	a2,a0
	move.l	H_ALLOC_SIZE(a4),d6
	bsr	crc32
	move.l	a3,a0
	move.l	H_RELOC_COUNT(a4),d6
	add.l	d6,d6
	add.l	d6,d6
	add.l	H_DATA_SIZE(a4),d6
	bsr	crc32
	not.l	d1
	cmp.l	H_BLOB_CRC(a4),d1
	beq.s	.crcok

	; CRC mismatch: COLOR00 = $F00 for ~50 frames
	lea	CUSTOM,a0
	moveq	#50-1,d2
.flash_frame:
	move.w	#$f00,COLOR00(a0)
	cmp.b	#$20,VHPOSR(a0)
	bne.s	.flash_frame
.flash_wait:
	move.w	#$f00,COLOR00(a0)
	cmp.b	#$20,VHPOSR(a0)
	beq.s	.flash_wait
	dbf	d2,.flash_frame
	bra	.fail

.crcok:
	; 5. relocate. Entry: bit 31 = the longword is in segment 1, bit 30 =
	;    add segment 1's base (else segment 0's), bits 29-0 = its offset
	move.l	a3,a0
	add.l	H_DATA_SIZE(a4),a0	; a0 = reloc table
	move.l	H_RELOC_COUNT(a4),d2
	bra.s	.relnext
.relloop:
	move.l	(a0)+,d0
	move.l	a2,a1
	bclr	#31,d0
	beq.s	.relloc
	move.l	a3,a1
.relloc:
	move.l	a2,d1
	bclr	#30,d0
	beq.s	.reltgt
	move.l	a3,d1
.reltgt:
	add.l	d1,0(a1,d0.l)
.relnext:
	subq.l	#1,d2
	bcc.s	.relloop

	; clear [base1+data_size, base1+data_size+bss_size) (over the relocs)
	move.l	a3,a0
	add.l	H_DATA_SIZE(a4),a0
	move.l	H_BSS_SIZE(a4),d2
	moveq	#0,d0
	bra.s	.bssnext
.bssloop:
	move.b	d0,(a0)+
.bssnext:
	subq.l	#1,d2
	bcc.s	.bssloop

	bsr	motor_off

	; caches
	cmp.w	#37,LIB_VERSION(a6)
	blo.s	.nocache
	jsr	_LVOCacheClearU(a6)
.nocache:

	; 6. call the payload
	move.l	a4,a0			; A0 = header copy (stays allocated)
	move.l	a2,a4
	add.l	H_ENTRY(a0),a4
	move.l	a5,a1			; A1 = boot IOStdReq
					; A2 = base0, A3 = base1, A6 = SysBase
	jsr	(a4)
	movem.l	(sp)+,d2-d7/a2-a6
	tst.l	d0
	bne.s	.retfail

	; normal bootblock exit: boot dos.library
	lea	dosname(pc),a1
	jsr	_LVOFindResident(a6)
	tst.l	d0
	beq.s	.retfail
	move.l	d0,a0
	move.l	RT_INIT(a0),a0
	moveq	#0,d0
	rts

; failure in steps 1-4: free what we allocated, return -1
.fail:
	move.l	a3,d0
	beq.s	.noseg1
	move.l	a3,a1
	move.l	H_DATA_ALLOC(a4),d0
	jsr	_LVOFreeMem(a6)
.noseg1:
	move.l	a2,d0
	beq.s	.noseg0
	move.l	a2,a1
	move.l	H_ALLOC_SIZE(a4),d0
	jsr	_LVOFreeMem(a6)
.noseg0:
	tst.l	d5
	beq.s	.nobounce
	move.l	d5,a1
	move.l	#BOUNCE,d0
	jsr	_LVOFreeMem(a6)
.nobounce:
	move.l	a4,d0
	beq.s	.nohdr
	move.l	a4,a1
	move.l	#512,d0
	jsr	_LVOFreeMem(a6)
.nohdr:
	bsr	motor_off
	movem.l	(sp)+,d2-d7/a2-a6
.retfail:
	moveq	#-1,d0
	rts

motor_off:
	move.w	#TD_MOTOR,IO_COMMAND(a5)
	clr.l	IO_LENGTH(a5)
	move.l	a5,a1
	jmp	_LVODoIO(a6)

; bounce_read: a0 = destination, d3 = length (a multiple of 512), d4 = disk
; offset; reads through the track buffer (d5). Returns Z set on success,
; d4 advanced past the data. Uses d0-d3, a0, a1.
bounce_read:
	tst.l	d3
	beq.s	.done
	move.l	#BOUNCE,d2
	cmp.l	d2,d3
	bhs.s	.full
	move.l	d3,d2
.full:
	move.l	a0,-(sp)
	move.l	d5,a0
	move.l	d2,d0
	move.l	d4,d1
	bsr.s	td_read
	move.l	(sp)+,a1		; flags kept
	bne.s	.done
	move.l	d5,a0
	move.l	d2,d0
	lsr.l	#2,d0
	subq.w	#1,d0
.copy:
	move.l	(a0)+,(a1)+
	dbf	d0,.copy
	move.l	a1,a0
	add.l	d2,d4
	sub.l	d2,d3
	bra.s	bounce_read
.done:
	rts

; crc32: a0 = data, d6 = byte count, d1 = running CRC. Uses d2-d4, d6.
crc32:
	move.l	#$edb88320,d3
	bra.s	.next
.byte:
	move.b	(a0)+,d2
	eor.b	d2,d1
	moveq	#7,d4
.bit:
	lsr.l	#1,d1
	bcc.s	.nox
	eor.l	d3,d1
.nox:
	dbf	d4,.bit
.next:
	subq.l	#1,d6
	bcc.s	.byte
	rts

; td_read: a0 = buffer, d0 = length, d1 = offset. Returns Z set on success.
td_read:
	move.l	a5,a1
	move.w	#CMD_READ,IO_COMMAND(a1)
	move.l	a0,IO_DATA(a1)
	move.l	d0,IO_LENGTH(a1)
	move.l	d1,IO_OFFSET(a1)
	jsr	_LVODoIO(a6)
	tst.b	IO_ERROR(a5)
	rts

dosname:
	dc.b	"dos.library",0
	even

bb_end:
	ifgt	bb_end-bb_start-1024
	fail	"bootblock larger than 1024 bytes"
	endc
