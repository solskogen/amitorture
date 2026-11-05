; timing module: fixed CPU loops (68000 instruction set).
; Every routine: ULONG fn(ULONG outer [, void *src, void *dst]), gcc stack
; ABI (args at 4(sp)...), result in d0, d2-d7/a2-a6 preserved.
; The inner loop body and count are fixed; `outer` only repeats it, so the
; host can compare ops/ticks between machines.

	xdef	_tm_dbra
	xdef	_tm_movereg
	xdef	_tm_movemem
	xdef	_tm_mulu
	xdef	_tm_divu
	xdef	_tm_branch

	section	.text,code

; 1000 x "dbra d2,*" per outer iteration
_tm_dbra:
	move.l	d2,-(sp)
	move.l	8(sp),d0
.o:	move.w	#999,d2
.i:	dbra	d2,.i
	subq.l	#1,d0
	bne.s	.o
	move.l	(sp)+,d2
	rts

; 100 x (8 x move.l reg,reg) per outer
_tm_movereg:
	movem.l	d2-d6,-(sp)
	move.l	24(sp),d0
	move.l	#$01234567,d3
	moveq	#0,d4
	moveq	#0,d5
	moveq	#0,d6
.o:	moveq	#99,d2
.i:	move.l	d3,d4
	move.l	d4,d5
	move.l	d5,d6
	move.l	d6,d3
	move.l	d3,d4
	move.l	d4,d5
	move.l	d5,d6
	move.l	d6,d3
	dbra	d2,.i
	subq.l	#1,d0
	bne.s	.o
	move.l	d3,d0
	movem.l	(sp)+,d2-d6
	rts

; copy 2048 bytes src -> dst (64 x 8 x move.l (a0)+,(a1)+) per outer
_tm_movemem:
	movem.l	d2/a2-a3,-(sp)
	move.l	16(sp),d0
	move.l	20(sp),a2
	move.l	24(sp),a3
.o:	move.l	a2,a0
	move.l	a3,a1
	moveq	#63,d2
.i:	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	dbra	d2,.i
	subq.l	#1,d0
	bne.s	.o
	movem.l	(sp)+,d2/a2-a3
	rts

; 100 x 8 x (move.w d5,d4 ; mulu.w d3,d4) per outer; d3=$5555 (8 one bits)
; result: $3333 * $5555 = $1110eeef
_tm_mulu:
	movem.l	d2-d5,-(sp)
	move.l	20(sp),d0
	move.l	#$5555,d3
	move.l	#$3333,d5
	moveq	#0,d4
.o:	moveq	#99,d2
.i:	move.w	d5,d4
	mulu.w	d3,d4
	move.w	d5,d4
	mulu.w	d3,d4
	move.w	d5,d4
	mulu.w	d3,d4
	move.w	d5,d4
	mulu.w	d3,d4
	move.w	d5,d4
	mulu.w	d3,d4
	move.w	d5,d4
	mulu.w	d3,d4
	move.w	d5,d4
	mulu.w	d3,d4
	move.w	d5,d4
	mulu.w	d3,d4
	dbra	d2,.i
	subq.l	#1,d0
	bne.s	.o
	move.l	d4,d0
	movem.l	(sp)+,d2-d5
	rts

; 100 x 8 x (move.l d5,d4 ; divu.w d3,d4) per outer
; $00123456 / $1234 -> quotient $0100, remainder $0056: d4 = $00560100
_tm_divu:
	movem.l	d2-d5,-(sp)
	move.l	20(sp),d0
	move.l	#$1234,d3
	move.l	#$00123456,d5
	moveq	#0,d4
.o:	moveq	#99,d2
.i:	move.l	d5,d4
	divu.w	d3,d4
	move.l	d5,d4
	divu.w	d3,d4
	move.l	d5,d4
	divu.w	d3,d4
	move.l	d5,d4
	divu.w	d3,d4
	move.l	d5,d4
	divu.w	d3,d4
	move.l	d5,d4
	divu.w	d3,d4
	move.l	d5,d4
	divu.w	d3,d4
	move.l	d5,d4
	divu.w	d3,d4
	dbra	d2,.i
	subq.l	#1,d0
	bne.s	.o
	move.l	d4,d0
	movem.l	(sp)+,d2-d5
	rts

; 256 data-dependent branch iterations per outer (Galois LFSR drives two
; conditional branches); returns the counter, modelled in timing.c
_tm_branch:
	movem.l	d2-d6,-(sp)
	move.l	24(sp),d0
	move.l	#$12345678,d3
	move.l	#$80200003,d6
	moveq	#0,d4
.o:	move.w	#255,d2
.i:	lsr.l	#1,d3
	bcc.s	.a
	eor.l	d6,d3
	addq.l	#3,d4
.a:	btst	#2,d3
	beq.s	.b
	addq.l	#1,d4
	bra.s	.c
.b:	subq.l	#1,d4
.c:	dbra	d2,.i
	subq.l	#1,d0
	bne.s	.o
	move.l	d4,d0
	movem.l	(sp)+,d2-d6
	rts

; ULONG tm_hscan(ULONG n): sample VHPOSR's H byte n*64 times, returns
; max<<8 | min. An LFSR adds a pseudo-random delay after each sample so the
; loop cannot phase-lock to the line (refresh-slot collisions on the chip
; bus can otherwise make every line sample the same H values).
	xdef	_tm_hscan
_tm_hscan:
	movem.l	d2-d6,-(sp)
	move.l	24(sp),d4
	lea	$dff006,a0
	moveq	#0,d1
	moveq	#-1,d2
	move.l	#$2545f491,d5
	move.l	#$80200003,d6
.o:	move.w	#63,d3
.i:	move.b	1(a0),d0
	cmp.b	d1,d0
	bls.s	.n
	move.b	d0,d1
.n:	cmp.b	d2,d0
	bcc.s	.m
	move.b	d0,d2
.m:	lsr.l	#1,d5
	bcc.s	.d1
	eor.l	d6,d5
	nop
.d1:	btst	#3,d5
	beq.s	.d2
	nop
	nop
	nop
.d2:	btst	#5,d5
	beq.s	.d3
	exg	d0,d0
.d3:	dbra	d3,.i
	subq.l	#1,d4
	bne.s	.o
	and.l	#$ff,d1
	and.l	#$ff,d2
	lsl.l	#8,d1
	or.l	d2,d1
	move.l	d1,d0
	movem.l	(sp)+,d2-d6
	rts
