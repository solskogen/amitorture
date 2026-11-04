; amitorture: 68010+ only code. Call only when AttnFlags has AFF_68010.
;
; ULONG amt_get_vbr(void) - MOVEC VBR,D0 in supervisor mode via Supervisor()

	xref	_SysBase
	xdef	_amt_get_vbr

_LVOSupervisor	equ	-30

	machine	68010
	section	.text,code

_amt_get_vbr:
	movem.l	a5/a6,-(sp)
	move.l	_SysBase,a6
	lea	.sv(pc),a5
	jsr	_LVOSupervisor(a6)
	movem.l	(sp)+,a5/a6
	rts

.sv:
	movec	vbr,d0
	rte
