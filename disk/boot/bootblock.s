; amitorture bootblock (CONTRACT.md section 2)
;
; Entry (from the Kickstart strap): A1 = trackdisk IOStdReq, A6 = SysBase.
; Exit:  D0 = 0, A0 = dos.library rt_Init  -> boot DOS
;        D0 = -1                           -> boot failure
;
; Loads the payload header (sector 2), allocates the payload, reads the blob,
; checks its CRC-32, relocates, clears the BSS, stops the motor, clears the
; caches (exec >= 37) and calls the payload entry.
;
; Only exec LVOs from KS1.2 are used, except CacheClearU (guarded).
; Before V36, trackdisk.device needs chip RAM buffers, so on exec < 36 every
; buffer is allocated with MEMF_CHIP added.
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
	sub.l	a2,a2			; a2 = payload base (none yet)

	; d7 = extra MEMF flags (MEMF_CHIP before V36)
	moveq	#0,d7
	cmp.w	#36,LIB_VERSION(a6)
	bhs.s	.newtd
	moveq	#MEMF_CHIP,d7
.newtd:

	; 1. header buffer + read sector 2
	move.l	#512,d0
	move.l	d7,d1			; MEMF_ANY (| CHIP)
	jsr	_LVOAllocMem(a6)
	tst.l	d0
	beq	.fail
	move.l	d0,a4

	move.l	a4,a0
	move.l	#512,d0
	move.l	#1024,d1
	bsr	td_read
	bne	.fail

	cmp.l	#'AMTP',H_MAGIC(a4)
	bne	.fail
	cmp.l	#1,H_VERSION(a4)
	bne	.fail

	; 2. payload memory
	move.l	H_ALLOC_SIZE(a4),d0
	beq	.fail
	move.l	H_ALLOC_FLAGS(a4),d1
	or.l	d7,d1
	jsr	_LVOAllocMem(a6)
	tst.l	d0
	beq	.fail
	move.l	d0,a2

	; 3. read the blob
	move.l	a2,a0
	move.l	H_BLOB_SECTORS(a4),d0
	beq	.fail
	moveq	#9,d1
	lsl.l	d1,d0			; * 512
	move.l	#1536,d1
	bsr	td_read
	bne	.fail

	; 4. CRC-32 (IEEE, reflected, poly $EDB88320) of image + reloc table
	move.l	H_RELOC_COUNT(a4),d6
	add.l	d6,d6
	add.l	d6,d6
	add.l	H_IMAGE_SIZE(a4),d6	; d6 = byte count
	move.l	a2,a0
	moveq	#-1,d1
	move.l	#$edb88320,d3
	tst.l	d6
	beq.s	.crcdone
.crcbyte:
	move.b	(a0)+,d2
	eor.b	d2,d1
	moveq	#7,d4
.crcbit:
	lsr.l	#1,d1
	bcc.s	.crcnox
	eor.l	d3,d1
.crcnox:
	dbf	d4,.crcbit
	subq.l	#1,d6
	bne.s	.crcbyte
.crcdone:
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
	; 5. relocate: for each r, *(u32 *)(base + r) += base
	move.l	a2,a0
	add.l	H_IMAGE_SIZE(a4),a0	; a0 = reloc table
	move.l	H_RELOC_COUNT(a4),d2
	move.l	a2,d1
	bra.s	.relnext
.relloop:
	move.l	(a0)+,d0
	add.l	d1,0(a2,d0.l)
.relnext:
	subq.l	#1,d2
	bcc.s	.relloop

	; clear [base+image_size, base+image_size+bss_size)
	move.l	a2,a0
	add.l	H_IMAGE_SIZE(a4),a0
	move.l	H_BSS_SIZE(a4),d2
	moveq	#0,d0
	bra.s	.bssnext
.bssloop:
	move.b	d0,(a0)+
.bssnext:
	subq.l	#1,d2
	bcc.s	.bssloop

	; motor off
	move.w	#TD_MOTOR,IO_COMMAND(a5)
	clr.l	IO_LENGTH(a5)
	move.l	a5,a1
	jsr	_LVODoIO(a6)

	; caches
	cmp.w	#37,LIB_VERSION(a6)
	blo.s	.nocache
	jsr	_LVOCacheClearU(a6)
.nocache:

	; 6. call the payload
	move.l	a2,a3
	add.l	H_ENTRY(a4),a3
	move.l	a4,a0			; A0 = header copy (stays allocated)
	move.l	a5,a1			; A1 = boot IOStdReq
					; A2 = base, A6 = SysBase
	jsr	(a3)
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
	move.l	a2,d0
	beq.s	.nobase
	move.l	a2,a1
	move.l	H_ALLOC_SIZE(a4),d0
	jsr	_LVOFreeMem(a6)
.nobase:
	move.l	a4,d0
	beq.s	.nohdr
	move.l	a4,a1
	move.l	#512,d0
	jsr	_LVOFreeMem(a6)
.nohdr:
	; motor off
	move.w	#TD_MOTOR,IO_COMMAND(a5)
	clr.l	IO_LENGTH(a5)
	move.l	a5,a1
	jsr	_LVODoIO(a6)
	movem.l	(sp)+,d2-d7/a2-a6
.retfail:
	moveq	#-1,d0
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
