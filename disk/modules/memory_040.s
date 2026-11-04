; memory: 68040/68060 helpers (called only on those CPUs).
;   void mem_move16(ULONG src, ULONG dst, ULONG lines)
;        MOVE16 (a0)+,(a1)+ lines times; the final a0/a1 go to mem_m16_a0/a1
;   void mem_cpusha_dc(void)      push and invalidate the data cache
;   void mem_cinvl_dc(ULONG a)    invalidate (no push) the data cache line of a
	machine	68040
	opt	o-
	xdef	_mem_move16
	xdef	_mem_cpusha_dc
	xdef	_mem_cinvl_dc
	xdef	_mem_m16_a0
	xdef	_mem_m16_a1
	section	.text,code
_mem_move16:
	move.l	4(sp),a0
	move.l	8(sp),a1
	move.l	12(sp),d0
	subq.l	#1,d0
.l:	move16	(a0)+,(a1)+
	dbf	d0,.l
	move.l	a0,_mem_m16_a0
	move.l	a1,_mem_m16_a1
	rts
_mem_cpusha_dc:
	nop
	cpusha	dc
	nop
	rts
_mem_cinvl_dc:
	move.l	4(sp),a0
	nop
	cinvl	dc,(a0)
	nop
	rts
	section	.bss,bss
_mem_m16_a0:	ds.l	1
_mem_m16_a1:	ds.l	1
