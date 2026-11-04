; memory: 68020+ helpers (called only on a 68020 or later).
;   ULONG mem_cas(ULONG addr, ULONG size, ULONG *dc, ULONG du, ULONG ccr)
;         CAS.B/.W/.L dc,du,(a0); *dc gets the compare register after;
;         returns the CCR
;   ULONG mem_cas2(ULONG *addr2, ULONG *dc2, const ULONG *du2, ULONG size, ULONG ccr)
;         CAS2.W/.L dc1:dc2,du1:du2,(a0):(a1) (68020/030/040 only)
;   void  mem_cacr_or(ULONG bits)       CACR |= bits (68020/030 CI/CD)
	machine	68020
	opt	o-
	xdef	_mem_cas
	xdef	_mem_cas2
	xdef	_mem_cacr_or
	section	.text,code
_mem_cas:
	movem.l	d2-d3,-(sp)
	move.l	12(sp),a0
	move.l	16(sp),d3
	move.l	20(sp),a1
	move.l	(a1),d0
	move.l	24(sp),d1
	move.l	28(sp),d2
	cmp.w	#1,d3
	bne.s	.w
	move.w	d2,ccr
	cas.b	d0,d1,(a0)
	bra.s	.out
.w:	cmp.w	#2,d3
	bne.s	.l
	move.w	d2,ccr
	cas.w	d0,d1,(a0)
	bra.s	.out
.l:	move.w	d2,ccr
	cas.l	d0,d1,(a0)
.out:	move.w	sr,d2
	move.l	d0,(a1)
	moveq	#$1f,d0
	and.w	d2,d0
	movem.l	(sp)+,d2-d3
	rts

_mem_cas2:
	movem.l	d2-d5/a2,-(sp)
	move.l	24(sp),a2
	move.l	(a2),a0
	move.l	4(a2),a1
	move.l	28(sp),a2
	move.l	(a2),d0
	move.l	4(a2),d1
	move.l	32(sp),a2
	move.l	(a2),d2
	move.l	4(a2),d3
	move.l	36(sp),d4
	move.l	40(sp),d5
	cmp.w	#2,d4
	bne.s	.l
	move.w	d5,ccr
	cas2.w	d0:d1,d2:d3,(a0):(a1)
	bra.s	.out
.l:	move.w	d5,ccr
	cas2.l	d0:d1,d2:d3,(a0):(a1)
.out:	move.w	sr,d5
	move.l	28(sp),a2
	move.l	d0,(a2)
	move.l	d1,4(a2)
	moveq	#$1f,d0
	and.w	d5,d0
	movem.l	(sp)+,d2-d5/a2
	rts

_mem_cacr_or:
	move.l	4(sp),d0
	movec	cacr,d1
	or.l	d0,d1
	movec	d1,cacr
	rts
