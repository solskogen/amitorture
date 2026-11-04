; dispatch (module 22): harness for the generated blocks (dispatch_tab.s).
;
;   void ds_run(void)
;
; Loads d0-d7/a0-a6 from ds_in, SR from ds_sr (S=1, IPL 0, the block's CCR)
; and SP from ds_in+60 (the block stack, M+$7FC), and enters the block at
; ds_entry with an RTS: the harness return address is stored at the block
; SP first, so the block's final RTS comes back to ds_ret. There the whole
; SR goes to ds_osr and d0-d7/a0-a7 to ds_out.
;
; ds_xstubs: handlers for vectors 2-15 and 32-63 while a block runs. Each
; stores all registers in ds_out, its vector number in ds_vec and the first
; 16 words of the exception frame in ds_frame, then abandons the frame and
; returns from ds_run. Vector 47 (TRAP #15) is the truncation point used to
; bisect a failing block: the C side replaces the first opcode word of
; instruction k with $4E4F in a copy of the block.
	opt	o-
	xdef	_ds_run
	xdef	_ds_in
	xdef	_ds_out
	xdef	_ds_sr
	xdef	_ds_osr
	xdef	_ds_entry
	xdef	_ds_vec
	xdef	_ds_frame
	xdef	_ds_xstubs
	section	.text,code
_ds_run:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	sp,ds_hsp
	clr.w	_ds_vec
	move.l	_ds_in+60,a0
	move.l	#ds_ret,(a0)
	move.l	_ds_entry,-(a0)
	move.l	a0,ds_bsp
	move.w	_ds_sr,sr
	movem.l	_ds_in,d0-d7/a0-a6
	move.l	ds_bsp,sp
	rts
ds_ret:
	move.w	sr,_ds_osr
	movem.l	d0-d7/a0-a7,_ds_out
	move.l	ds_hsp,sp
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; one 6-byte stub per vector: move.w #n,-(sp)? No: the stack must stay as
; the exception left it, so each stub stores its number with an absolute
; move and branches to the common tail.
xstub	macro
	move.w	#\1,ds_vtmp
	bra.w	ds_xcommon
	endm
_ds_xstubs:
	xstub	2
	xstub	3
	xstub	4
	xstub	5
	xstub	6
	xstub	7
	xstub	8
	xstub	9
	xstub	10
	xstub	11
	xstub	12
	xstub	13
	xstub	14
	xstub	15
	xstub	32
	xstub	33
	xstub	34
	xstub	35
	xstub	36
	xstub	37
	xstub	38
	xstub	39
	xstub	40
	xstub	41
	xstub	42
	xstub	43
	xstub	44
	xstub	45
	xstub	46
	xstub	47
	xstub	48
	xstub	49
	xstub	50
	xstub	51
	xstub	52
	xstub	53
	xstub	54
	xstub	55
	xstub	56
	xstub	57
	xstub	58
	xstub	59
	xstub	60
	xstub	61
	xstub	62
	xstub	63
ds_xcommon:
	movem.l	d0-d7/a0-a7,_ds_out
	move.w	ds_vtmp,_ds_vec
	lea	_ds_frame,a0
	moveq	#15,d0
.cp:	move.w	(sp)+,(a0)+
	dbf	d0,.cp
	move.l	ds_hsp,sp
	movem.l	(sp)+,d2-d7/a2-a6
	rts

	section	.bss,bss
	cnop	0,4
_ds_in:		ds.l	16
_ds_out:	ds.l	16
_ds_entry:	ds.l	1
ds_hsp:		ds.l	1
ds_bsp:		ds.l	1
_ds_frame:	ds.w	16
_ds_sr:		ds.w	1
_ds_osr:	ds.w	1
_ds_vec:	ds.w	1
ds_vtmp:	ds.w	1
