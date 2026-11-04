; dispatch: cache maintenance after the C side patched a copy of a block
; (supervisor mode; called only on the CPU family named).
;   void ds_cacr_or(ULONG bits)   68020/030: CACR |= bits (CI, CD)
;   void ds_cpusha(int is060)     68040/060: CPUSHA BC (+ 060 CABC: clear the
;                                 branch cache, MC68060UM 5.3)
	machine	68040
	opt	o-
	xdef	_ds_cacr_or
	xdef	_ds_cpusha
	section	.text,code
_ds_cacr_or:
	move.l	4(sp),d0
	movec	cacr,d1
	or.l	d0,d1
	movec	d1,cacr
	rts
_ds_cpusha:
	nop
	cpusha	bc
	nop
	tst.l	4(sp)
	beq.s	.done
	movec	cacr,d1
	or.l	#$00400000,d1
	movec	d1,cacr
.done:	rts
