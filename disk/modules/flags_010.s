; flags module, 68010+ only: MOVE from CCR (not in the 68000).
	machine	68010
	section	.text,code

; void fl_movefromccr_table(UWORD *out)
; for v 0..31: MOVE to CCR, MOVE CCR,Dn (stored), MOVE CCR,(An)+, MOVE SR,(An)+
	xdef	_fl_movefromccr_table
_fl_movefromccr_table:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0
	moveq	#0,d1
.l:	move.l	#$a5a5a5a5,d2
	move.w	d1,ccr
	move.w	ccr,d2
	move.w	ccr,2(a0)
	move.w	sr,4(a0)
	move.w	d2,(a0)
	addq.l	#6,a0
	addq.w	#1,d1
	cmp.w	#32,d1
	bne.s	.l
	movem.l	(sp)+,d2-d7/a2-a6
	rts
