; smc module: 68020/68030 cache control registers. Only called when the
; probe says 68020 or later (CAAR only on 68020/68030).
	machine	68020
	section	.text,code
	xdef	_smc_cacr_get
	xdef	_smc_cacr_set
	xdef	_smc_caar_set
_smc_cacr_get:
	movec	cacr,d0
	rts
_smc_cacr_set:
	move.l	4(sp),d0
	movec	d0,cacr
	rts
_smc_caar_set:
	move.l	4(sp),d0
	movec	d0,caar
	rts
