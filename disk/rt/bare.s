;
; amitorture bare-phase support (CONTRACT.md section 10)
;
;   int  amt_try(void (*fn)(void))      run fn, catch amt_abort(); nestable
;   void amt_abort(int code)            unwind to the innermost amt_try
;   amt_catchall                        default handler for vectors 2..63
;   amt_isr_l1..l6                      autovector handlers: level 3 VERTB is
;                                       the runtime clock/heartbeat/watchdog,
;                                       everything else goes to C hooks
;   UWORD amt_get_sr(void), void amt_set_sr(UWORD)
;   void amt_run_on_stack(void (*fn)(void), void *top)
;
; All of it runs in supervisor mode (the bare phase is entered with
; SuperState()). Handlers save d0-d1/a0-a1 and call C (gcc keeps d2-d7/a2-a6).

	xdef	_amt_try
	xdef	_amt_abort
	xdef	_amt_abort_d0
	xdef	_amt_get_sr
	xdef	_amt_set_sr
	xdef	_amt_run_on_stack
	xdef	_amt_supervisor_run
	xref	_SysBase
	xdef	_amt_stub_table
	xdef	_amt_isr_table
	xdef	amt_catchall

	xref	_amt_last_exc
	xref	_amt_bus_frame_68000
	xref	_amt_frames
	xref	_amt_wd_left
	xref	_amt_wd_hard
	xref	_amt_wd_armed
	xref	_amt_mbox
	xref	_amt_irq_hook

	section	.text,code

; ---------------------------------------------------------------- try/abort
; try frame on the stack: d2-d7/a2-a6 (44), sr.w (2), previous jmp sp (4)
_amt_try:
	movem.l	d2-d7/a2-a6,-(sp)
	move.w	sr,-(sp)
	move.l	jmp_sp,-(sp)
	move.l	sp,jmp_sp
	move.l	4+2+44+4(sp),a0		; fn
	jsr	(a0)
	moveq	#0,d0
try_out:
	move.l	(sp)+,jmp_sp
	move.w	(sp)+,sr
	movem.l	(sp)+,d2-d7/a2-a6
	rts

_amt_abort:
	move.l	4(sp),d0
_amt_abort_d0:				; code in d0, callable from handlers
	move.w	#$2700,sr
	move.l	jmp_sp,d1
	beq.s	.nojmp
	move.l	d1,sp
	tst.l	d0
	bne.s	try_out
	moveq	#3,d0
	bra.s	try_out
.nojmp:	bra.s	.nojmp			; no try active: hang (cannot happen)

; ---------------------------------------------------------------- sr
_amt_get_sr:
	moveq	#0,d0
	move.w	sr,d0
	rts
_amt_set_sr:
	move.w	6(sp),sr
	rts

; ---------------------------------------------------------------- stack
_amt_run_on_stack:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0		; fn
	move.l	52(sp),a1		; top
	move.l	sp,old_sp
	move.l	a1,sp
	jsr	(a0)
	move.l	old_sp,sp
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; void amt_supervisor_run(void (*fn)(void), void *top)
; Runs fn in supervisor mode on our own stack through exec Supervisor(), and
; returns with RTE, which pops whatever frame this CPU built (6 bytes on the
; 68000, format $0 8 bytes on 010+). Unlike SuperState()/UserState() this
; does not depend on the Kickstart version knowing the CPU's frame format.
_amt_supervisor_run:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),sv_fn
	move.l	52(sp),sv_top
	move.l	_SysBase,a6
	lea	.sv(pc),a5
	jsr	-30(a6)			; Supervisor()
	movem.l	(sp)+,d2-d7/a2-a6
	rts
.sv:	move.l	sp,sv_ssp
	move.l	sv_top,sp
	move.l	sv_fn,a0
	jsr	(a0)
	move.l	sv_ssp,sp
	rte

; ---------------------------------------------------------------- catch-all
; stack at entry: vec.w, then the exception frame
amt_catchall:
	move.w	#$2700,sr
	movem.l	d0/a0-a1,-(sp)
	lea	_amt_last_exc,a0
	move.w	12(sp),(a0)+		; vector
	lea	14(sp),a1		; frame start
	cmp.w	#3,-2(a0)
	bhi.s	.grp12
	tst.w	_amt_bus_frame_68000
	beq.s	.grp12
	; vectors 2/3: on the 68000 the frame is status,addr.l,ir,sr,pc.l
	move.w	8(a1),(a0)+		; sr
	move.l	10(a1),(a0)+		; pc
	bra.s	.raw
.grp12:	move.w	(a1),(a0)+		; sr
	move.l	2(a1),(a0)+		; pc
.raw:	moveq	#15,d0
.cp:	move.w	(a1)+,(a0)+
	dbf	d0,.cp
	movem.l	(sp)+,d0/a0-a1
	moveq	#1,d0
	bra	_amt_abort_d0

amt_stub_2:	move.w	#2,-(sp)
	bra.w	amt_catchall
amt_stub_3:	move.w	#3,-(sp)
	bra.w	amt_catchall
amt_stub_4:	move.w	#4,-(sp)
	bra.w	amt_catchall
amt_stub_5:	move.w	#5,-(sp)
	bra.w	amt_catchall
amt_stub_6:	move.w	#6,-(sp)
	bra.w	amt_catchall
amt_stub_7:	move.w	#7,-(sp)
	bra.w	amt_catchall
amt_stub_8:	move.w	#8,-(sp)
	bra.w	amt_catchall
amt_stub_9:	move.w	#9,-(sp)
	bra.w	amt_catchall
amt_stub_10:	move.w	#10,-(sp)
	bra.w	amt_catchall
amt_stub_11:	move.w	#11,-(sp)
	bra.w	amt_catchall
amt_stub_12:	move.w	#12,-(sp)
	bra.w	amt_catchall
amt_stub_13:	move.w	#13,-(sp)
	bra.w	amt_catchall
amt_stub_14:	move.w	#14,-(sp)
	bra.w	amt_catchall
amt_stub_15:	move.w	#15,-(sp)
	bra.w	amt_catchall
amt_stub_16:	move.w	#16,-(sp)
	bra.w	amt_catchall
amt_stub_17:	move.w	#17,-(sp)
	bra.w	amt_catchall
amt_stub_18:	move.w	#18,-(sp)
	bra.w	amt_catchall
amt_stub_19:	move.w	#19,-(sp)
	bra.w	amt_catchall
amt_stub_20:	move.w	#20,-(sp)
	bra.w	amt_catchall
amt_stub_21:	move.w	#21,-(sp)
	bra.w	amt_catchall
amt_stub_22:	move.w	#22,-(sp)
	bra.w	amt_catchall
amt_stub_23:	move.w	#23,-(sp)
	bra.w	amt_catchall
amt_stub_24:	move.w	#24,-(sp)
	bra.w	amt_catchall
amt_stub_25:	move.w	#25,-(sp)
	bra.w	amt_catchall
amt_stub_26:	move.w	#26,-(sp)
	bra.w	amt_catchall
amt_stub_27:	move.w	#27,-(sp)
	bra.w	amt_catchall
amt_stub_28:	move.w	#28,-(sp)
	bra.w	amt_catchall
amt_stub_29:	move.w	#29,-(sp)
	bra.w	amt_catchall
amt_stub_30:	move.w	#30,-(sp)
	bra.w	amt_catchall
amt_stub_31:	move.w	#31,-(sp)
	bra.w	amt_catchall
amt_stub_32:	move.w	#32,-(sp)
	bra.w	amt_catchall
amt_stub_33:	move.w	#33,-(sp)
	bra.w	amt_catchall
amt_stub_34:	move.w	#34,-(sp)
	bra.w	amt_catchall
amt_stub_35:	move.w	#35,-(sp)
	bra.w	amt_catchall
amt_stub_36:	move.w	#36,-(sp)
	bra.w	amt_catchall
amt_stub_37:	move.w	#37,-(sp)
	bra.w	amt_catchall
amt_stub_38:	move.w	#38,-(sp)
	bra.w	amt_catchall
amt_stub_39:	move.w	#39,-(sp)
	bra.w	amt_catchall
amt_stub_40:	move.w	#40,-(sp)
	bra.w	amt_catchall
amt_stub_41:	move.w	#41,-(sp)
	bra.w	amt_catchall
amt_stub_42:	move.w	#42,-(sp)
	bra.w	amt_catchall
amt_stub_43:	move.w	#43,-(sp)
	bra.w	amt_catchall
amt_stub_44:	move.w	#44,-(sp)
	bra.w	amt_catchall
amt_stub_45:	move.w	#45,-(sp)
	bra.w	amt_catchall
amt_stub_46:	move.w	#46,-(sp)
	bra.w	amt_catchall
amt_stub_47:	move.w	#47,-(sp)
	bra.w	amt_catchall
amt_stub_48:	move.w	#48,-(sp)
	bra.w	amt_catchall
amt_stub_49:	move.w	#49,-(sp)
	bra.w	amt_catchall
amt_stub_50:	move.w	#50,-(sp)
	bra.w	amt_catchall
amt_stub_51:	move.w	#51,-(sp)
	bra.w	amt_catchall
amt_stub_52:	move.w	#52,-(sp)
	bra.w	amt_catchall
amt_stub_53:	move.w	#53,-(sp)
	bra.w	amt_catchall
amt_stub_54:	move.w	#54,-(sp)
	bra.w	amt_catchall
amt_stub_55:	move.w	#55,-(sp)
	bra.w	amt_catchall
amt_stub_56:	move.w	#56,-(sp)
	bra.w	amt_catchall
amt_stub_57:	move.w	#57,-(sp)
	bra.w	amt_catchall
amt_stub_58:	move.w	#58,-(sp)
	bra.w	amt_catchall
amt_stub_59:	move.w	#59,-(sp)
	bra.w	amt_catchall
amt_stub_60:	move.w	#60,-(sp)
	bra.w	amt_catchall
amt_stub_61:	move.w	#61,-(sp)
	bra.w	amt_catchall
amt_stub_62:	move.w	#62,-(sp)
	bra.w	amt_catchall
amt_stub_63:	move.w	#63,-(sp)
	bra.w	amt_catchall

; ---------------------------------------------------------------- interrupts
; Level n handler: if amt_irq_hook[n] is set, call it with INTREQR & INTENAR;
; level 3 additionally owns VERTB (frame counter, heartbeat, watchdog).
ISR	macro
amt_isr_l\1:
	movem.l	d0-d1/a0-a1,-(sp)
	move.w	$dff01e,d0
	and.w	$dff01c,d0
	moveq	#\1,d1
	bra	isr_common
	endm

	ISR	1
	ISR	2
	ISR	3
	ISR	4
	ISR	5
	ISR	6

isr_common:
	; d0 = INTREQR & INTENAR, d1 = level. Keep only this level's bits so
	; one level never touches (or drops) another level's requests.
	move.w	d1,a1
	add.w	d1,d1
	lea	level_mask(pc),a0
	and.w	0(a0,d1.w),d0
	move.w	a1,d1
	tst.w	d0
	beq.w	.done			; request already gone (or another level's)
	cmp.w	#3,d1
	bne.s	.hook
	btst	#5,d0			; VERTB
	beq.s	.hook
	; Clearing VERTB and counting the frame must be one step for the
	; level 4-6 handlers that read the clock (amt_ticks, paula's now_calc):
	; they add a frame while VERTB is still pending. Between the clear and
	; the increment they saw "cleared, not counted" and stamped their event
	; one frame early (paula.dsksyn_irq, t_dsksyn_cck = -45728 on NTSC).
	move.w	sr,-(sp)
	or.w	#$0700,sr
	move.w	#$0020,$dff09c
	move.w	#$0020,$dff09c
	addq.l	#1,_amt_frames
	move.w	(sp)+,sr
	move.l	_amt_mbox,a0
	move.l	a0,d1
	beq.s	.nomb
	addq.l	#1,24(a0)		; heartbeat
.nomb:	tst.w	_amt_wd_armed
	beq.s	.wdok
	subq.l	#1,_amt_wd_hard		; whole-module limit: never kicked
	beq.s	.wdfire
	subq.l	#1,_amt_wd_left		; progress limit: kicked per case
	bne.s	.wdok
.wdfire:
	clr.w	_amt_wd_armed
	movem.l	(sp)+,d0-d1/a0-a1
	moveq	#2,d0
	bra	_amt_abort_d0
.wdok:	moveq	#3,d1
	and.w	#$ffdf,d0		; COPER/BLIT left for the hook
	beq.s	.done
.hook:	lea	_amt_irq_hook,a0
	lsl.w	#2,d1
	move.l	0(a0,d1.w),d1
	beq.s	.spurious
	move.l	d1,a0
	and.l	#$ffff,d0
	move.l	d0,-(sp)		; this level's pending bits only
	jsr	(a0)
	addq.l	#4,sp
.done:	movem.l	(sp)+,d0-d1/a0-a1
	rte
.spurious:
	; a request on this level nobody hooked: clear only this level's bits
	move.w	d0,$dff09c
	move.w	d0,$dff09c
	movem.l	(sp)+,d0-d1/a0-a1
	rte

level_mask:				; INTREQ bits per IPL level (HRM table 7-1)
	dc.w	$0000,$0007,$0008,$0070,$0780,$1800,$2000

	section	.data,data
_amt_stub_table:			; vectors 2..63
	dc.l	amt_stub_2
	dc.l	amt_stub_3
	dc.l	amt_stub_4
	dc.l	amt_stub_5
	dc.l	amt_stub_6
	dc.l	amt_stub_7
	dc.l	amt_stub_8
	dc.l	amt_stub_9
	dc.l	amt_stub_10
	dc.l	amt_stub_11
	dc.l	amt_stub_12
	dc.l	amt_stub_13
	dc.l	amt_stub_14
	dc.l	amt_stub_15
	dc.l	amt_stub_16
	dc.l	amt_stub_17
	dc.l	amt_stub_18
	dc.l	amt_stub_19
	dc.l	amt_stub_20
	dc.l	amt_stub_21
	dc.l	amt_stub_22
	dc.l	amt_stub_23
	dc.l	amt_stub_24
	dc.l	amt_stub_25
	dc.l	amt_stub_26
	dc.l	amt_stub_27
	dc.l	amt_stub_28
	dc.l	amt_stub_29
	dc.l	amt_stub_30
	dc.l	amt_stub_31
	dc.l	amt_stub_32
	dc.l	amt_stub_33
	dc.l	amt_stub_34
	dc.l	amt_stub_35
	dc.l	amt_stub_36
	dc.l	amt_stub_37
	dc.l	amt_stub_38
	dc.l	amt_stub_39
	dc.l	amt_stub_40
	dc.l	amt_stub_41
	dc.l	amt_stub_42
	dc.l	amt_stub_43
	dc.l	amt_stub_44
	dc.l	amt_stub_45
	dc.l	amt_stub_46
	dc.l	amt_stub_47
	dc.l	amt_stub_48
	dc.l	amt_stub_49
	dc.l	amt_stub_50
	dc.l	amt_stub_51
	dc.l	amt_stub_52
	dc.l	amt_stub_53
	dc.l	amt_stub_54
	dc.l	amt_stub_55
	dc.l	amt_stub_56
	dc.l	amt_stub_57
	dc.l	amt_stub_58
	dc.l	amt_stub_59
	dc.l	amt_stub_60
	dc.l	amt_stub_61
	dc.l	amt_stub_62
	dc.l	amt_stub_63
_amt_isr_table:				; levels 1..6
	dc.l	amt_isr_l1,amt_isr_l2,amt_isr_l3,amt_isr_l4,amt_isr_l5,amt_isr_l6

	section	.bss,bss
jmp_sp:	ds.l	1
old_sp:	ds.l	1
sv_fn:	ds.l	1
sv_top:	ds.l	1
sv_ssp:	ds.l	1
