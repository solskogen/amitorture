; cetiming: timed loops (68000 instruction set), copied by cetiming.c into
; chip RAM and run from there. Position independent: only PC-relative
; branches, absolute addresses are hardware registers.
;
; Every routine: ULONG fn(ULONG count, struct cet_blk *b), gcc stack ABI,
; d2-d7/a2-a6 preserved. The loop body runs count+1 times (dbra) between a
; CIA-B timer A one-shot start (CRA = START|RUNMODE|LOAD, latch $FFFF set by
; the caller) and stop (CRA = RUNMODE). Returns the remaining timer count
; (TAHI:TALO); the caller computes elapsed = $FFFF - result.
;
; struct cet_blk (cetiming.c): +0 target address, +4/+8/+12/+16 inputs
; d1/d2/d3/d5, +20/+24/+28/+32 outputs d0/d1/d3/d5.
;
; The 68020 I-cache fetches longwords, so the cost of a cached loop depends
; on where its words sit (see docs/cases/cetiming.md). Each loop body
; starts at a fixed offset mod 4 from _cet_blob_start, set by CET_ALIGN;
; cetiming.c checks the offsets (case layout).

	opt	o-
	section	.text,code

	xdef	_cet_blob_start
	xdef	_cet_blob_end
	xdef	_cet_chip_read
	xdef	_cet_chip_write_a2
	xdef	_cet_chip_write_a0
	xdef	_cet_move_reg
	xdef	_cet_lsl
	xdef	_cet_mulu
	xdef	_cet_dbra
	xdef	_cet_move_pair
	xdef	_cet_move_pair_dep
	xdef	_cet_l_chip_read
	xdef	_cet_l_chip_write_a2
	xdef	_cet_l_chip_write_a0
	xdef	_cet_l_move_reg
	xdef	_cet_l_lsl
	xdef	_cet_l_mulu
	xdef	_cet_l_dbra
	xdef	_cet_l_move_pair
	xdef	_cet_l_move_pair_dep

CIAB	equ	$bfd000
CRA	equ	$e00
TALO	equ	$400
TAHI	equ	$500

CET_ENTER macro
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),d6
	move.l	52(sp),a5
	lea	CIAB,a4
	move.l	(a5),a0
	move.l	4(a5),d1
	move.l	8(a5),d2
	move.l	12(a5),d3
	move.l	16(a5),d5
	moveq	#0,d0
	move.b	#$19,CRA(a4)
	endm

; pad with one NOP (executed once, before the loop) so the next
; instruction is at offset \1 (0 or 2) mod 4 from the blob start
CET_ALIGN macro
cet_al\@:
	ifne	((cet_al\@-_cet_blob_start)&3)-\1
	nop
	endif
	endm

CET_LEAVE macro
	move.b	#$08,CRA(a4)
	movem.l	d0/d1/d3/d5,20(a5)
	moveq	#0,d0
	move.b	TAHI(a4),d0
	lsl.w	#8,d0
	move.b	TALO(a4),d0
	movem.l	(sp)+,d2-d7/a2-a6
	rts
	endm

_cet_blob_start:

	cnop	0,4
_cet_chip_read:
	CET_ENTER
	CET_ALIGN 0
_cet_l_chip_read:
	move.w	(a0),d0
	dbra	d6,_cet_l_chip_read
	CET_LEAVE

	cnop	0,4
_cet_chip_write_a2:
	CET_ENTER
	CET_ALIGN 2
_cet_l_chip_write_a2:
	move.w	d1,(a0)
	dbra	d6,_cet_l_chip_write_a2
	CET_LEAVE

	cnop	0,4
_cet_chip_write_a0:
	CET_ENTER
	CET_ALIGN 0
_cet_l_chip_write_a0:
	move.w	d1,(a0)
	dbra	d6,_cet_l_chip_write_a0
	CET_LEAVE

	cnop	0,4
_cet_move_reg:
	CET_ENTER
	CET_ALIGN 2
_cet_l_move_reg:
	move.w	d2,d0
	dbra	d6,_cet_l_move_reg
	CET_LEAVE

	cnop	0,4
_cet_lsl:
	CET_ENTER
	CET_ALIGN 2
_cet_l_lsl:
	lsl.l	#8,d3
	dbra	d6,_cet_l_lsl
	CET_LEAVE

	cnop	0,4
_cet_mulu:
	CET_ENTER
	CET_ALIGN 2
_cet_l_mulu:
	mulu.w	#$5555,d5
	dbra	d6,_cet_l_mulu
	CET_LEAVE

	cnop	0,4
_cet_dbra:
	CET_ENTER
	CET_ALIGN 0
_cet_l_dbra:
	dbra	d6,_cet_l_dbra
	CET_LEAVE

	cnop	0,4
_cet_move_pair:
	CET_ENTER
	CET_ALIGN 0
_cet_l_move_pair:
	move.w	d2,d0
	move.w	d3,d1
	dbra	d6,_cet_l_move_pair
	CET_LEAVE

	cnop	0,4
_cet_move_pair_dep:
	CET_ENTER
	CET_ALIGN 2
_cet_l_move_pair_dep:
	move.w	d2,d0
	move.w	d0,d1
	dbra	d6,_cet_l_move_pair_dep
	CET_LEAVE

	cnop	0,4
_cet_blob_end:
