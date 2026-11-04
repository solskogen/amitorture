; dmacontention: cache maintenance (supervisor; called only on the family named)
;   void dc_cacr_or(ULONG bits)   68020/030: CACR |= bits (CI = $8, CD = $800)
;   void dc_cpusha(void)          68040/060: push and invalidate both caches
;   ULONG dc_cacr_get(void), void dc_cacr_set(ULONG)   MOVEC CACR (020+)
	machine	68040
	opt	o-
	xdef	_dc_cacr_or
	xdef	_dc_cpusha
	section	.text,code
_dc_cacr_or:
	move.l	4(sp),d0
	movec	cacr,d1
	or.l	d0,d1
	movec	d1,cacr
	rts
_dc_cpusha:
	nop
	cpusha	bc
	nop
	rts
	xdef	_dc_cacr_get
	xdef	_dc_cacr_set
_dc_cacr_get:
	movec	cacr,d0
	rts
_dc_cacr_set:
	move.l	4(sp),d0
	movec	d0,cacr
	rts
