; cetiming: instruction cache flush after the loops are copied to chip RAM
; (supervisor; each is called only on the family named)
;   void cet_cacr_or(ULONG bits)   68020/030: CACR |= bits (CI = $8)
;   void cet_cpusha(void)          68040/060: push and invalidate both caches
	machine	68040
	opt	o-
	xdef	_cet_cacr_or
	xdef	_cet_cpusha
	section	.text,code
_cet_cacr_or:
	move.l	4(sp),d0
	movec	cacr,d1
	or.l	d0,d1
	movec	d1,cacr
	rts
_cet_cpusha:
	nop
	cpusha	bc
	nop
	rts
