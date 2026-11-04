; cpu_basic: run one generated snippet with a full register/CCR state.
;
;   void cb_exec(void)
;
; Loads d0-d7/a0-a6 from cb_in and the CCR from cb_ccr, calls cb_entry (the
; snippet ends with rts), then stores d0-d7/a0-a6 to cb_out, the whole SR to
; cb_osr and the stack pointer seen after the snippet to cb_osp (cb_isp is
; the value it must equal). Only MOVEM/MOVE-to-CCR/RTS sit between the state
; load and the snippet, none of which changes the loaded CCR.
	opt	o-
	xdef	_cb_exec
	xdef	_cb_in
	xdef	_cb_out
	xdef	_cb_ccr
	xdef	_cb_osr
	xdef	_cb_entry
	xdef	_cb_isp
	xdef	_cb_osp
	section	.text,code
_cb_exec:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	sp,_cb_isp
	pea	.ret(pc)
	move.l	_cb_entry,-(sp)
	movem.l	_cb_in,d0-d7/a0-a6
	move.w	_cb_ccr,ccr
	rts
.ret:
	move.w	sr,_cb_osr
	movem.l	d0-d7/a0-a6,_cb_out
	move.l	sp,_cb_osp
	move.l	_cb_isp,sp
	movem.l	(sp)+,d2-d7/a2-a6
	rts

	section	.bss,bss
	cnop	0,4
_cb_in:		ds.l	15
_cb_out:	ds.l	15
_cb_entry:	ds.l	1
_cb_isp:	ds.l	1
_cb_osp:	ds.l	1
_cb_ccr:	ds.w	1
_cb_osr:	ds.w	1
