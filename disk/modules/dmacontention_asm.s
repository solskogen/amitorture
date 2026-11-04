; dmacontention (module 23): position-independent code that the C side
; copies into the chip pool and runs from there (so instruction fetches use
; the chip bus too).
;   void dc_loop(ULONG *buf256, ULONG iters, ULONG *res3)
;        fixed integer workload with stores to and loads from a 1 KB chip
;        buffer (zeroed first); res3 = final x, y, sum (expected values
;        computed on the host for DC_ITERS iterations)
;   void dc_sample(UWORD *buf, ULONG n16, volatile UWORD *reg)
;        n16 x 16 times: MOVE.W (reg),(buf)+  (a custom register sampled
;        into chip RAM as fast as the bus allows)
	opt	o-
	xdef	_dc_code
	xdef	_dc_code_end
	xdef	_dc_loop_off
	xdef	_dc_sample_off
	section	.text,code
	cnop	0,4
_dc_code:
dc_loop:
	movem.l	d2-d7/a2,-(sp)
	move.l	32(sp),a0
	move.l	36(sp),d7
	move.l	#$12345678,d0
	move.l	#$9abcdef0,d1
	moveq	#0,d2
	moveq	#0,d3
.l:	rol.l	#5,d0
	add.l	d1,d0
	eor.l	d0,d1
	move.l	d0,0(a0,d2.w)
	add.w	#$1c,d2
	and.w	#$3fc,d2
	add.l	0(a0,d2.w),d3
	subq.l	#1,d7
	bne.s	.l
	move.l	40(sp),a0
	movem.l	d0/d1/d3,(a0)
	movem.l	(sp)+,d2-d7/a2
	rts
dc_sample:
	move.l	4(sp),a0
	move.l	8(sp),d0
	move.l	12(sp),a1
	subq.l	#1,d0
.s:	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	move.w	(a1),(a0)+
	dbf	d0,.s
	rts
	cnop	0,4
_dc_code_end:
_dc_loop_off:	dc.l	dc_loop-_dc_code
_dc_sample_off:	dc.l	dc_sample-_dc_code
