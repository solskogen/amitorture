; bench harness helpers (harness.h): 68060 PCR and an FPU reset.
	machine	68020
	fpu	1
	opt	o-

	section	.text,code

	xdef	_bh_pcr_get
	xdef	_bh_pcr_set
	xdef	_bh_fpu_reset

_bh_pcr_get:
	dc.w	$4e7a,$0808		; movec pcr,d0
	rts
_bh_pcr_set:
	move.l	4(sp),d0
	dc.w	$4e7b,$0808		; movec d0,pcr
	rts

_bh_fpu_reset:				; FRESTORE of a null frame: FPU reset
	frestore nullframe(pc)
	rts

	cnop	0,4
nullframe:	dc.l	0,0,0
