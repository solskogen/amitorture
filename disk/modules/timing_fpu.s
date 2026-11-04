; timing module: FPU loop (called only when the probe found an FPU).
; ULONG tm_fpu(ULONG outer): 100 x 4 x (fmul.x fp1,fp0 ; fadd.x fp2,fp0)
; per outer with fp1 = 0.5, fp2 = 1.0: x <- x/2 + 1 converges to exactly
; 2.0 in extended precision (round to nearest), so the result is 2.
	xdef	_tm_fpu
	section	.text,code
_tm_fpu:
	move.l	d2,-(sp)
	fmovem.x fp0-fp2,-(sp)
	fmove.l	fpcr,-(sp)
	move.l	48(sp),d0
	fmove.l	#0,fpcr
	fmove.l	#0,fp0
	fmove.l	#1,fp1
	fdiv.l	#2,fp1
	fmove.l	#1,fp2
.o:	moveq	#99,d2
.i:	fmul.x	fp1,fp0
	fadd.x	fp2,fp0
	fmul.x	fp1,fp0
	fadd.x	fp2,fp0
	fmul.x	fp1,fp0
	fadd.x	fp2,fp0
	fmul.x	fp1,fp0
	fadd.x	fp2,fp0
	dbra	d2,.i
	subq.l	#1,d0
	bne.s	.o
	fmove.l	fp0,d0
	fmove.l	(sp)+,fpcr
	fmovem.x (sp)+,fp0-fp2
	move.l	(sp)+,d2
	rts
