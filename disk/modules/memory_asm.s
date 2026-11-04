; memory (module 21): access helpers with exact instructions (68000 code).
;   ULONG mem_rd(ULONG addr, ULONG size)             MOVE.B/.W/.L (a0),d0
;   void  mem_wr(ULONG addr, ULONG size, ULONG v)    MOVE.B/.W/.L d0,(a0)
;   void  mem_movem_st(ULONG addr, const ULONG *v14, ULONG word)
;         MOVEM.L/.W d0-d7/a0-a5,(a6) with the registers loaded from v14
;   void  mem_movem_ld(ULONG addr, ULONG *out14, ULONG word)
;         MOVEM.L/.W (a6),d0-d7/a0-a5, registers stored to out14
;   ULONG mem_tas(ULONG addr, ULONG ccr)             TAS (a0); returns CCR
;   ULONG mem_crc(ULONG crc, ULONG addr, ULONG len, ULONG longs, const ULONG *tab)
;         CRC-32 (zlib) update over [addr, addr+len) read with MOVE.B
;         (longs=0) or MOVE.L (longs=1; len a multiple of 4)
;   void  mem_bench(ULONG addr, ULONG passes, ULONG mode)
;         over 2 KB per pass: 0 = MOVE.L reads, 1 = MOVE.L writes,
;         2 = MOVEM.L copy of the first KB to the second
	opt	o-
	xdef	_mem_rd
	xdef	_mem_wr
	xdef	_mem_movem_st
	xdef	_mem_movem_ld
	xdef	_mem_tas
	xdef	_mem_bench
	xdef	_mem_crc
	section	.text,code
_mem_rd:
	move.l	4(sp),a0
	move.l	8(sp),d1
	moveq	#0,d0
	cmp.w	#1,d1
	bne.s	.w
	move.b	(a0),d0
	rts
.w:	cmp.w	#2,d1
	bne.s	.l
	move.w	(a0),d0
	rts
.l:	move.l	(a0),d0
	rts

_mem_wr:
	move.l	4(sp),a0
	move.l	8(sp),d1
	move.l	12(sp),d0
	cmp.w	#1,d1
	bne.s	.w
	move.b	d0,(a0)
	rts
.w:	cmp.w	#2,d1
	bne.s	.l
	move.w	d0,(a0)
	rts
.l:	move.l	d0,(a0)
	rts

_mem_movem_st:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a6
	move.l	52(sp),a0
	tst.l	56(sp)
	bne.s	.w
	movem.l	(a0),d0-d7/a0-a5
	movem.l	d0-d7/a0-a5,(a6)
	bra.s	.x
.w:	movem.l	(a0),d0-d7/a0-a5
	movem.w	d0-d7/a0-a5,(a6)
.x:	movem.l	(sp)+,d2-d7/a2-a6
	rts

_mem_movem_ld:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a6
	tst.l	56(sp)
	bne.s	.w
	movem.l	(a6),d0-d7/a0-a5
	bra.s	.st
.w:	movem.w	(a6),d0-d7/a0-a5
.st:	move.l	52(sp),a6
	movem.l	d0-d7/a0-a5,(a6)
	movem.l	(sp)+,d2-d7/a2-a6
	rts

_mem_tas:
	move.l	4(sp),a0
	move.l	8(sp),d0
	move.w	d0,ccr
	tas	(a0)
	move.w	sr,d0
	and.l	#$1f,d0
	rts

_mem_bench:
	movem.l	d2-d7/a2-a3,-(sp)
	move.l	36(sp),a2
	move.l	40(sp),d6
	move.l	44(sp),d7
	subq.l	#1,d6
.pass:	move.l	a2,a0
	cmp.w	#1,d7
	beq.s	.wr
	bhi.s	.cp
	moveq	#31,d1
.rd:	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	move.l	(a0)+,d0
	dbf	d1,.rd
	bra.s	.next
.wr:	moveq	#31,d1
	move.l	d6,d0
.wl:	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	move.l	d0,(a0)+
	dbf	d1,.wl
	bra.s	.next
.cp:	lea	1024(a2),a1
	moveq	#31,d1
.cl:	movem.l	(a0)+,d0/d2-d5/a3
	movem.l	d0/d2-d5/a3,(a1)
	lea	24(a1),a1
	movem.l	(a0)+,d0/d2
	movem.l	d0/d2,(a1)
	lea	8(a1),a1
	dbf	d1,.cl
.next:	dbf	d6,.pass
	movem.l	(sp)+,d2-d7/a2-a3
	rts

_mem_crc:
	movem.l	d2-d5/a2,-(sp)
	move.l	24(sp),d0
	not.l	d0
	move.l	28(sp),a0
	move.l	32(sp),d1
	move.l	40(sp),a1
	tst.l	36(sp)
	bne.s	.lw
.lb:	moveq	#0,d2
	move.b	(a0)+,d2
	eor.b	d0,d2
	add.w	d2,d2
	add.w	d2,d2
	lsr.l	#8,d0
	move.l	0(a1,d2.w),d3
	eor.l	d3,d0
	subq.l	#1,d1
	bne.s	.lb
	bra.s	.out
.lw:	lsr.l	#2,d1
.ll:	move.l	(a0)+,d4
	moveq	#3,d5
.lby:	rol.l	#8,d4
	moveq	#0,d2
	move.b	d4,d2
	eor.b	d0,d2
	add.w	d2,d2
	add.w	d2,d2
	lsr.l	#8,d0
	move.l	0(a1,d2.w),d3
	eor.l	d3,d0
	dbf	d5,.lby
	subq.l	#1,d1
	bne.s	.ll
.out:	not.l	d0
	movem.l	(sp)+,d2-d5/a2
	rts
