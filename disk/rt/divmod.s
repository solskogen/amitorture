; 32-bit division helpers for gcc on the 68000 (the toolchain keeps these
; in libnix, which we do not link). Arguments on the stack, result in D0.

	xdef	___udivsi3
	xdef	___umodsi3
	xdef	___divsi3
	xdef	___modsi3

	section	.text,code

___udivsi3:
	move.l	4(sp),d0
	move.l	8(sp),d1
	bra.s	udivmod

___umodsi3:
	move.l	4(sp),d0
	move.l	8(sp),d1
	bsr.s	udivmod
	move.l	d1,d0
	rts

___divsi3:
	move.l	d2,-(sp)
	move.l	8(sp),d0
	move.l	12(sp),d1
	move.l	d0,d2
	eor.l	d1,d2			; sign of the quotient
	tst.l	d0
	bpl.s	.d1
	neg.l	d0
.d1:	tst.l	d1
	bpl.s	.d2
	neg.l	d1
.d2:	bsr.s	udivmod
	tst.l	d2
	bpl.s	.d3
	neg.l	d0
.d3:	move.l	(sp)+,d2
	rts

___modsi3:
	move.l	d2,-(sp)
	move.l	8(sp),d0
	move.l	12(sp),d1
	move.l	d0,d2			; remainder takes the dividend's sign
	bpl.s	.m1
	neg.l	d0
.m1:	tst.l	d1
	bpl.s	.m2
	neg.l	d1
.m2:	bsr.s	udivmod
	move.l	d1,d0
	tst.l	d2
	bpl.s	.m3
	neg.l	d0
.m3:	move.l	(sp)+,d2
	rts

; d0 / d1 -> d0 = quotient, d1 = remainder (division by 0 gives -1, d0)
udivmod:
	movem.l	d2-d3,-(sp)
	move.l	d1,d3
	moveq	#0,d1
	moveq	#31,d2
.loop:
	add.l	d0,d0
	addx.l	d1,d1
	bcs.s	.sub
	cmp.l	d3,d1
	bcs.s	.next
.sub:
	sub.l	d3,d1
	addq.l	#1,d0
.next:
	dbf	d2,.loop
	movem.l	(sp)+,d2-d3
	rts

; ULONG __mulsi3(ULONG a, ULONG b) - 32x32->32 multiply for the 68000
; (a = ah:al, b = bh:bl; result = al*bl + ((ah*bl + al*bh) << 16))
	xdef	___mulsi3
___mulsi3:
	move.w	4(sp),d0	; ah
	mulu.w	10(sp),d0	; ah*bl
	move.w	6(sp),d1	; al
	mulu.w	8(sp),d1	; al*bh
	add.w	d1,d0
	swap	d0
	clr.w	d0
	move.w	6(sp),d1	; al
	mulu.w	10(sp),d1	; al*bl
	add.l	d1,d0
	rts
