; amitorture payload entry (CONTRACT.md sections 3 and 14)
;
; long amt_entry(SysBase:A6, hdr:A0, bootio:A1, base:A2)
; Saves the registers, stores the arguments in globals and calls amt_main().
; amt_main's int result goes back to the bootblock in D0. The bootblock
; jumps to amt_entry (mkadf takes entry_offset from the _amt_entry symbol).
;
; amt_cli_entry, at offset 0 of the image (the first hunk), is the AmigaDOS
; CLI start used when the payload runs as a program (CD boot: the runner
; wraps the ADF's image in a hunk executable and s/startup-sequence runs it).
; AmigaDOS starts a program at the beginning of its first hunk with
; A0/D0 = arguments and no SysBase in A6, so the shim loads SysBase from 4,
; points A0 at the header copy embedded below (the runner writes the ADF's
; sector 2 there: run parameters and build id), passes bootio = NULL and
; returns RETURN_OK (0) to the CLI if amt_main ever returns.

	xref	_SysBase
	xref	_amt_hdr
	xref	_amt_bootio
	xref	_amt_base
	xref	_amt_main
	xdef	_amt_entry
	xdef	amt_entry
	xdef	_amt_cli_hdr

	section	.text,code

amt_cli_entry:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	4.w,a6
	lea	_amt_cli_hdr,a0
	sub.l	a1,a1
	lea	amt_cli_entry(pc),a2
	bsr.s	amt_entry
	movem.l	(sp)+,d2-d7/a2-a6
	moveq	#0,d0
	rts

amt_entry:
_amt_entry:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	a6,_SysBase
	move.l	a0,_amt_hdr
	move.l	a1,_amt_bootio
	move.l	a2,_amt_base
	jsr	_amt_main
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; CLI header slot: the 16-byte marker the runner searches for, then 512
; bytes that receive a copy of the payload header (CONTRACT §1). Zero (no
; "PARM" block) means default run parameters.
	section	.data,data
	cnop	0,4
	dc.b	"AMT-CLI-HEADER!!"
_amt_cli_hdr:
	dcb.b	512,0
