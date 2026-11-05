;
; exceptions module (id 4): recording handlers and the test snippets.
;
; This object is assembled for the 68000 (the Makefile passes -m68000). Every
; instruction whose exact encoding is under test is written as dc.w, so the
; same object holds the snippets for every CPU; C only calls a snippet when
; the probe says it is meaningful for the detected variant.
;
; Recording (xr_*)
;   xr_call(fn)       runs the snippet fn with d0-d7 = xr_in[], a0 = xr_ina0,
;                     a1 = xr_ina1 and SR = xr_srin. Returns the vector of
;                     the exception that ended it (0 = it returned normally).
;   xr_stub_table     one stub per vector 2..63. A stub records the SR the
;                     handler starts with and its vector number, then:
;                       xr_active = 0: chains to the previous handler
;                                      (xr_old[vec], the runtime catch-all);
;                       xr_mode   = 0: records regs + 30 frame words + SSP and
;                                      resumes after the xr_call (no RTE, so
;                                      any frame, also group 0, is fine);
;                       xr_mode   = 1: appends {vec, hsr, ssp, 12 frame words}
;                                      to xr_log and returns with RTE. If
;                                      vec == xr_skipvec the stacked PC is
;                                      advanced by 2 first; if the stacked PC
;                                      == xr_stop_pc the T1/T0 bits of the
;                                      stacked SR are cleared (end of trace).
;
	xdef	_xr_call
	xdef	_xr_get_usp,_xr_set_usp
	xdef	_xr_stub_table
	xdef	_xr_vec,_xr_hsr,_xr_mode,_xr_nlog,_xr_skipvec,_xr_srin
	xdef	_xr_ssp,_xr_spbase,_xr_stop_pc,_xr_usp,_xr_ina0,_xr_ina1
	xdef	_xr_frame,_xr_regs,_xr_in,_xr_out,_xr_old,_xr_log,_xr_wbdone
	xdef	_xn_ssp1,_xn_ssp1b,_xn_ssp2,_xn_hsr1,_xn_hsr2,_xn_f1,_xn_f1b,_xn_f2
	xdef	_xh_nest1,_xh_nest1_at,_xh_nest1_nx,_xh_nest2

XLOGMAX	equ	40

	section	.text,code

; ------------------------------------------------------------------ xr_call
_xr_call:
	movem.l	d2-d7/a2-a6,-(sp)
	move.w	sr,-(sp)
	move.l	sp,_xr_save_sp
	move.l	50(sp),a2		; fn
	move.l	sp,d0
	subq.l	#4,d0			; SP inside the snippet (after jsr)
	move.l	d0,_xr_spbase
	clr.w	_xr_vec
	clr.w	_xr_wbdone
	clr.w	_xr_nlog
	move.w	#1,_xr_active
	movem.l	_xr_in,d0-d7
	move.l	_xr_ina1,a1
	move.l	_xr_ina0,a0
	move.w	_xr_srin,sr
	jsr	(a2)
	movem.l	d0-d7,_xr_out
xr_cont:
	move.w	#$2700,sr
	clr.w	_xr_active
	move.l	_xr_save_sp,sp
	move.w	(sp)+,sr
	movem.l	(sp)+,d2-d7/a2-a6
	moveq	#0,d0
	move.w	_xr_vec,d0
	rts

_xr_get_usp:
	move.l	usp,a0
	move.l	a0,d0
	rts
_xr_set_usp:
	move.l	4(sp),a0
	move.l	a0,usp
	rts

; ------------------------------------------------------------------ stubs
; complete one 68040 write-back from the copied format $7 frame: \1 status
; word, \2 address, \3 data (byte offsets in _xr_frame). WBnS bit 7 = valid,
; bits 6-5 size (00 long, 01 byte, 10 word, 11 line: not done here).
; \4 marks WB1: memory-aligned data, unlike right-justified WB2/WB3.
XWB	macro
	move.w	_xr_frame+\1,d0
	btst	#7,d0
	beq.s	.n\@
	move.l	_xr_frame+\2,a1
	move.l	_xr_frame+\3,d1
	if	\4
	move.l	a1,d0
	and.w	#3,d0
	lsl.w	#3,d0
	rol.l	d0,d1
	move.w	_xr_frame+\1,d0
	and.w	#$60,d0
	beq.s	.wb1done\@
	cmp.w	#$60,d0
	beq.s	.n\@
	swap	d1
	cmp.w	#$20,d0
	bne.s	.wb1done\@
	lsr.w	#8,d1
.wb1done\@:
	move.w	_xr_frame+\1,d0
	endif
	and.w	#$60,d0
	beq.s	.l\@
	cmp.w	#$20,d0
	beq.s	.b\@
	cmp.w	#$40,d0
	bne.s	.n\@
	move.w	d1,(a1)
	bra.s	.c\@
.b\@:	move.b	d1,(a1)
	bra.s	.c\@
.l\@:	move.l	d1,(a1)
.c\@:	addq.w	#1,_xr_wbdone
.n\@:
	endm

XSTUB	macro
xstub_\1:
	move.w	sr,_xr_hsr_tmp
	move.w	#\1,_xr_cur
	bra.w	xr_common
	endm

	XSTUB	2
	XSTUB	3
	XSTUB	4
	XSTUB	5
	XSTUB	6
	XSTUB	7
	XSTUB	8
	XSTUB	9
	XSTUB	10
	XSTUB	11
	XSTUB	12
	XSTUB	13
	XSTUB	14
	XSTUB	15
	XSTUB	16
	XSTUB	17
	XSTUB	18
	XSTUB	19
	XSTUB	20
	XSTUB	21
	XSTUB	22
	XSTUB	23
	XSTUB	24
	XSTUB	25
	XSTUB	26
	XSTUB	27
	XSTUB	28
	XSTUB	29
	XSTUB	30
	XSTUB	31
	XSTUB	32
	XSTUB	33
	XSTUB	34
	XSTUB	35
	XSTUB	36
	XSTUB	37
	XSTUB	38
	XSTUB	39
	XSTUB	40
	XSTUB	41
	XSTUB	42
	XSTUB	43
	XSTUB	44
	XSTUB	45
	XSTUB	46
	XSTUB	47
	XSTUB	48
	XSTUB	49
	XSTUB	50
	XSTUB	51
	XSTUB	52
	XSTUB	53
	XSTUB	54
	XSTUB	55
	XSTUB	56
	XSTUB	57
	XSTUB	58
	XSTUB	59
	XSTUB	60
	XSTUB	61
	XSTUB	62
	XSTUB	63

xr_common:
	tst.w	_xr_active
	bne.s	.act
	; not ours: chain to the previous handler with the frame untouched
	subq.l	#4,sp
	move.l	a0,-(sp)
	move.w	_xr_cur,a0
	add.l	a0,a0
	add.l	a0,a0
	add.l	#_xr_old,a0
	move.l	(a0),4(sp)
	move.l	(sp)+,a0
	rts
.act:	tst.w	_xr_mode
	bne.w	xr_logit
	movem.l	d0-d7/a0-a6,_xr_regs
	move.w	_xr_cur,_xr_vec
	move.w	_xr_hsr_tmp,_xr_hsr
	move.l	sp,_xr_ssp
	move.l	sp,a1
	lea	_xr_frame,a0
	moveq	#29,d0
.cp:	move.w	(a1)+,(a0)+
	dbf	d0,.cp
	; 68040 access error (format $7): complete the pending write-backs
	; WB1-WB3 before resuming without RTE, or the stores still in the
	; CPU's buffers (e.g. the stack writes of xr_call) would be lost
	; (MC68040UM 8.2.1 access error stack frame: WBnS valid bits). Only
	; bus_error_unmapped_read (unsafe mode, 020+) gets here with vector 2.
	cmp.w	#2,_xr_cur
	bne.w	xr_resume
	move.w	_xr_frame+6,d0
	and.w	#$f000,d0
	cmp.w	#$7000,d0
	bne.w	xr_resume
	XWB	18,40,44,1		; WB1S, WB1A, WB1D
	XWB	16,32,36,0		; WB2S, WB2A, WB2D
	XWB	14,24,28,0		; WB3S, WB3A, WB3D
xr_resume:
	move.w	#$2700,sr
	move.l	_xr_save_sp,sp
	bra	xr_cont

xr_logit:
	movem.l	d0-d1/a0-a1,-(sp)
	lea	16(sp),a1		; the frame
	move.w	_xr_nlog,d0
	cmp.w	#XLOGMAX,d0
	bhs.s	.over
	addq.w	#1,_xr_nlog
	lsl.w	#5,d0
	lea	_xr_log,a0
	adda.w	d0,a0
	move.w	_xr_cur,(a0)+
	move.w	_xr_hsr_tmp,(a0)+
	move.l	a1,(a0)+
	moveq	#11,d1
.cp:	move.w	(a1)+,(a0)+
	dbf	d1,.cp
	move.w	_xr_cur,d0
	cmp.w	_xr_skipvec,d0
	bne.s	.nskip
	addq.l	#2,16+2(sp)		; resume after a one-word instruction
.nskip:	move.l	16+2(sp),d0
	cmp.l	_xr_stop_pc,d0
	beq.s	.stop
	cmp.w	#XLOGMAX-8,_xr_nlog
	blo.s	.ok
.stop:	and.w	#$3fff,16(sp)		; clear T1/T0: tracing ends here
.ok:	movem.l	(sp)+,d0-d1/a0-a1
	rte
.over:	movem.l	(sp)+,d0-d1/a0-a1	; runaway (e.g. an RTE loop): give up
	move.w	#$ffff,_xr_vec
	bra	xr_resume

; ------------------------------------------------------------------ nested
; TRAP #1 handler that takes TRAP #2 itself; both record their frames.
_xh_nest1:
	move.w	sr,_xn_hsr1
	move.l	sp,_xn_ssp1
	move.l	(sp),_xn_f1
	move.l	4(sp),_xn_f1+4
	move.l	8(sp),_xn_f1+8
	move.l	12(sp),_xn_f1+12
	move.w	#$2708,sr
_xh_nest1_at:
	dc.w	$4e42			; trap #2
_xh_nest1_nx:
	move.l	sp,_xn_ssp1b
	move.l	(sp),_xn_f1b
	move.l	4(sp),_xn_f1b+4
	move.l	8(sp),_xn_f1b+8
	move.l	12(sp),_xn_f1b+12
	rte
_xh_nest2:
	move.w	sr,_xn_hsr2
	move.l	sp,_xn_ssp2
	move.l	(sp),_xn_f2
	move.l	4(sp),_xn_f2+4
	move.l	8(sp),_xn_f2+8
	move.l	12(sp),_xn_f2+12
	rte

; ================================================================== snippets
; Naming: _xs_<name> = entry, _xs_<name>_at = the instruction under test,
; _xs_<name>_nx = the instruction after it. Unless noted, entry == _at.

XS	macro
	xdef	_xs_\1,_xs_\1_at,_xs_\1_nx
	endm

; drop to user mode with USP = xr_usp (IPL and CCR kept)
USERMODE macro
	move.l	_xr_usp,a6
	move.l	a6,usp
	andi.w	#$dfff,sr
	endm

	cnop	0,4
; TRAP #0..15: 16 entries of 4 bytes (trap #n / rts)
	xdef	_xs_trap
_xs_trap:
N	set	0
	rept	16
	dc.w	$4e40+N
	rts
N	set	N+1
	endr

; TRAPcc (68020+): 3 forms x 16 conditions, 8 bytes per entry
;   form 0: TRAPcc        (1 word)
;   form 1: TRAPcc.W #imm (2 words)
;   form 2: TRAPcc.L #imm (3 words)
	xdef	_xs_tcc
_xs_tcc:
N	set	0
	rept	16
	dc.w	$50fc+(N<<8)
	rts
	dc.w	0,0
N	set	N+1
	endr
N	set	0
	rept	16
	dc.w	$50fa+(N<<8),$1234
	rts
	dc.w	0
N	set	N+1
	endr
N	set	0
	rept	16
	dc.w	$50fb+(N<<8),$1234,$5678
	rts
N	set	N+1
	endr

; illegal / line-A / line-F samples: 8 bytes each (op, then ILLEGAL x3 so a
; wrongly "legal" op is still caught right behind it). Same order as the C
; table ill_tab[].
	xdef	_xs_ill
_xs_ill:
ILL	macro
	dc.w	\1,$4afc,$4afc,$4afc
	endm
	ILL	$4afc
	ILL	$41c0
	ILL	$4ec0
	ILL	$4ec8
	ILL	$4e7c
	ILL	$4e7d
	ILL	$4e7e
	ILL	$4e7f
	ILL	$4afa
	ILL	$4afb
	ILL	$4e74
	ILL	$4e7a
	ILL	$42c0
	ILL	$0e90
	ILL	$49c0
	; LINK.L a0,#0 if wrongly decoded on the 68000/010 (the $4afc filler as
	; a displacement would move SP by $4afc4afc and the trap frame with it)
	dc.w	$4808,$0000,$0000,$4afc
	ILL	$4101
	ILL	$4c41
	ILL	$50fc
	ILL	$00d0
	ILL	$06c0
	ILL	$a000
	ILL	$afff
	ILL	$ffff
	xdef	_xs_ill_end
_xs_ill_end:

; TRAPV
	XS	trapv
_xs_trapv:
_xs_trapv_at:
	dc.w	$4e76
_xs_trapv_nx:
	rts

; CHK.W D1,D0
	XS	chkw
_xs_chkw:
_xs_chkw_at:
	dc.w	$4181
_xs_chkw_nx:
	move.w	sr,d7
	rts
; CHK.W #100,D0
	XS	chkwi
_xs_chkwi:
_xs_chkwi_at:
	dc.w	$41bc,100
_xs_chkwi_nx:
	move.w	sr,d7
	rts
; CHK.L D1,D0 (020+)
	XS	chkl
_xs_chkl:
_xs_chkl_at:
	dc.w	$4101
_xs_chkl_nx:
	move.w	sr,d7
	rts

; CHK2/CMP2 (020-040; 060: unimplemented integer), bounds at (a0)
	XS	chk2l
_xs_chk2l:
_xs_chk2l_at:
	dc.w	$04d0,$0800		; chk2.l (a0),d0
_xs_chk2l_nx:
	move.w	sr,d7
	rts
	XS	cmp2l
_xs_cmp2l:
_xs_cmp2l_at:
	dc.w	$04d0,$0000		; cmp2.l (a0),d0
_xs_cmp2l_nx:
	move.w	sr,d7
	rts
	XS	chk2b
_xs_chk2b:
_xs_chk2b_at:
	dc.w	$00d0,$0800		; chk2.b (a0),d0
_xs_chk2b_nx:
	move.w	sr,d7
	rts
	XS	cmp2w
_xs_cmp2w:
_xs_cmp2w_at:
	dc.w	$02d0,$0000		; cmp2.w (a0),d0
_xs_cmp2w_nx:
	move.w	sr,d7
	rts
	XS	chk2wa
_xs_chk2wa:
_xs_chk2wa_at:
	dc.w	$02d0,$9800		; chk2.w (a0),a1
_xs_chk2wa_nx:
	move.w	sr,d7
	rts

; divide by zero (dividend d0, divisor d1, d2 = remainder/high register)
SNIP	macro
	XS	\1
_xs_\1:
_xs_\1_at:
	dc.w	\2
_xs_\1_nx:
	move.w	sr,d7
	rts
	endm
	SNIP	divuw,$80c1			; divu.w d1,d0
	SNIP	divsw,$81c1			; divs.w d1,d0
	SNIP	divuwi,<$80fc,$0000>		; divu.w #0,d0
	SNIP	divswi,<$81fc,$0000>		; divs.w #0,d0
	SNIP	divul,<$4c41,$0000>		; divu.l d1,d0
	SNIP	divsl,<$4c41,$0800>		; divs.l d1,d0
	SNIP	divull,<$4c41,$0002>		; divul.l d1,d2:d0
	SNIP	divsll,<$4c41,$0802>		; divsl.l d1,d2:d0
	SNIP	divul64,<$4c41,$0402>		; divu.l d1,d2:d0 (64/32)
	SNIP	divsl64,<$4c41,$0c02>		; divs.l d1,d2:d0 (64/32)
	SNIP	divuli,<$4c7c,$0000,$0000,$0000> ; divu.l #0,d0
; 060 unimplemented integer instructions (also run nowhere else)
	SNIP	mulul64,<$4c01,$0402>		; mulu.l d1,d2:d0
	SNIP	mulsl64,<$4c01,$0c02>		; muls.l d1,d2:d0
	SNIP	movepw,<$0108,$0000>		; movep.w 0(a0),d0
	SNIP	movepl,<$01c8,$0000>		; movep.l d0,0(a0)
	SNIP	cas2w,<$0cfc,$8080,$90c1>	; cas2.w d0:d1,d2:d3,(a0):(a1)
	SNIP	cas2l,<$0efc,$8080,$90c1>	; cas2.l d0:d1,d2:d3,(a0):(a1)
	SNIP	casw,<$0cd0,$0040>		; cas.w d0,d1,(a0)
; FNOP (an FPU instruction; FPU-less CPUs only)
	SNIP	fnop,<$f280,$0000>

; ---------------------------------------------------- privilege (user mode)
UPRIV	macro
	XS	\1
_xs_\1:
	USERMODE
_xs_\1_at:
	dc.w	\2
_xs_\1_nx:
	dc.w	$4afc			; escape back to supervisor
	endm
	UPRIV	umovesr,<$46fc,$2700>	; move.w #$2700,sr
	UPRIV	umovefsr,$40c0		; move.w sr,d0
	UPRIV	uandisr,<$027c,$ffff>	; andi.w #$ffff,sr
	UPRIV	uorisr,<$007c,$0000>	; ori.w #0,sr
	UPRIV	ueorisr,<$0a7c,$0000>	; eori.w #0,sr
	UPRIV	umoveusp,$4e68		; move.l usp,a0
	UPRIV	umovec,<$4e7a,$0801>	; movec vbr,d0
	UPRIV	umoves,<$0e90,$0000>	; moves.l (a0),d0
	UPRIV	urte,$4e73		; rte
	UPRIV	utrap,$4e43		; trap #3 (user frame)
	UPRIV	uaerr,$3010		; move.w (a0),d0 (68000/010: a0 odd)

; ---------------------------------------------------- address error (000/010)
	SNIP	aerw,$3010		; move.w (a0),d0
	SNIP	aerl,$2010		; move.l (a0),d0
	SNIP	aeww,$3081		; move.w d1,(a0)
	SNIP	aewl,$2081		; move.l d1,(a0)
	SNIP	aejmp,$4ed0		; jmp (a0)
	xdef	_xs_aetgt
	cnop	0,2
_xs_aetgt:
	dc.w	$4afc,$4afc,$4afc,$4afc
; bus error probe: move.l (a0),d0
	SNIP	berr,$2010

; misaligned data (020+): a0 = pool + 1
	xdef	_xs_mis
_xs_mis:
	move.l	(a0),d0			; pool+1..4
	move.w	2(a0),d1		; pool+3..4
	move.l	d2,4(a0)		; pool+5..8
	move.l	4(a0),d3
	move.w	d4,10(a0)		; pool+11..12
	move.l	8(a0),d5		; pool+9..12
	addq.w	#1,12(a0)		; pool+13..14, read-modify-write
	rts

; ---------------------------------------------------- trace
; T1: every instruction
	xdef	_xs_tr1,_xs_tr1_a,_xs_tr1_b,_xs_tr1_c,_xs_tr1_d,_xs_tr1_e
	xdef	_xs_tr1_f,_xs_tr1_g,_xs_tr1_h,_xs_tr1_sub,_xs_tr1_s1
_xs_tr1:
	move.w	#$a700,sr
_xs_tr1_a:
	moveq	#1,d0
_xs_tr1_b:
	add.l	d0,d0
_xs_tr1_c:
	dc.w	$6002			; bra.s _xs_tr1_e
_xs_tr1_d:
	nop
_xs_tr1_e:
	jsr	_xs_tr1_sub
_xs_tr1_f:
	lea	12(a0),a0
_xs_tr1_g:
	move.w	#$2700,sr
_xs_tr1_h:
	rts
_xs_tr1_sub:
	nop
_xs_tr1_s1:
	rts

; T0: change of flow only (020-040)
	xdef	_xs_tr0,_xs_tr0_a,_xs_tr0_b,_xs_tr0_c,_xs_tr0_d,_xs_tr0_e
	xdef	_xs_tr0_f,_xs_tr0_g,_xs_tr0_x,_xs_tr0_end,_xs_tr0_sub,_xs_tr0_s1
_xs_tr0:
	ori.w	#$4000,sr
_xs_tr0_a:
	moveq	#1,d0
_xs_tr0_b:
	dc.w	$6002			; bra.s _xs_tr0_d
_xs_tr0_c:
	nop
_xs_tr0_d:
	add.l	d0,d0
_xs_tr0_e:
	jsr	_xs_tr0_sub
_xs_tr0_f:
	nop
_xs_tr0_g:
	dc.w	$6002			; bra.s _xs_tr0_end
_xs_tr0_x:
	nop
_xs_tr0_end:
	rts
_xs_tr0_sub:
	nop
_xs_tr0_s1:
	rts

; trace + TRAP #5, trace + ILLEGAL
	xdef	_xs_trt,_xs_trt_at,_xs_trt_nx,_xs_trt_n2
_xs_trt:
	move.w	#$a700,sr
_xs_trt_at:
	dc.w	$4e45			; trap #5
_xs_trt_nx:
	move.w	#$2700,sr
_xs_trt_n2:
	rts
	xdef	_xs_tri,_xs_tri_at,_xs_tri_nx,_xs_tri_n2
_xs_tri:
	move.w	#$a700,sr
_xs_tri_at:
	dc.w	$4afc			; illegal
_xs_tri_nx:
	move.w	#$2700,sr
_xs_tri_n2:
	rts

; nested: TRAP #1 whose handler takes TRAP #2
	XS	nest
_xs_nest:
_xs_nest_at:
	dc.w	$4e41
_xs_nest_nx:
	move.w	sr,d7
	rts

; ---------------------------------------------------- RTE
; d0 = format/vector word, d1 = extra words to push (zeros), d2 = SR to
; return with, d4 != 0: put a safe format $0 frame (SR $2700, PC =
; _xs_rte1_bad, an ILLEGAL) right below the frame, so a CPU that wrongly
; treats the frame as a throwaway frame lands on that ILLEGAL in supervisor
; mode. Out: d7 = SR after the RTE, d6 = SP after - SP before.
	XS	rte
_xs_rte:
	move.l	sp,a1
	bra.s	.chk
.push:	clr.w	-(sp)
.chk:	dbf	d1,.push
	tst.l	d4
	beq.s	.nosafe
	clr.w	-(sp)
	pea	_xs_rte1_bad
	move.w	#$2700,-(sp)
.nosafe:
	move.w	d0,-(sp)
	pea	_xs_rte_nx
	move.w	d2,-(sp)
_xs_rte_at:
	rte
_xs_rte_nx:
	move.w	sr,d7
	move.l	sp,d6
	sub.l	a1,d6
	move.l	a1,sp
	rts
; format $1 (throwaway) on top of a format $0 frame; d3 = throwaway SR
	XS	rte1
	xdef	_xs_rte1_bad
_xs_rte1:
	move.l	sp,a1
	clr.w	-(sp)
	pea	_xs_rte1_nx
	move.w	d2,-(sp)
	move.w	#$1000,-(sp)
	pea	_xs_rte1_bad
	move.w	d3,-(sp)
_xs_rte1_at:
	rte
_xs_rte1_bad:
	dc.w	$4afc
_xs_rte1_nx:
	move.w	sr,d7
	move.l	sp,d6
	sub.l	a1,d6
	move.l	a1,sp
	rts

; ------------------------------------------------------------------ data
	section	.bss,bss
_xr_save_sp:	ds.l	1
_xr_spbase:	ds.l	1
_xr_ssp:	ds.l	1
_xr_stop_pc:	ds.l	1
_xr_usp:	ds.l	1
_xr_ina0:	ds.l	1
_xr_ina1:	ds.l	1
_xr_in:		ds.l	8
_xr_out:	ds.l	8
_xr_regs:	ds.l	15
_xr_old:	ds.l	64
_xr_vec:	ds.w	1
_xr_wbdone:	ds.w	1
_xr_hsr:	ds.w	1
_xr_hsr_tmp:	ds.w	1
_xr_cur:	ds.w	1
_xr_mode:	ds.w	1
_xr_active:	ds.w	1
_xr_nlog:	ds.w	1
_xr_skipvec:	ds.w	1
_xr_srin:	ds.w	1
_xr_frame:	ds.w	30
_xr_log:	ds.b	32*XLOGMAX
_xn_hsr1:	ds.w	1
_xn_hsr2:	ds.w	1
_xn_ssp1:	ds.l	1
_xn_ssp1b:	ds.l	1
_xn_ssp2:	ds.l	1
_xn_f1:		ds.w	8
_xn_f1b:	ds.w	8
_xn_f2:		ds.w	8

	section	.data,data
_xr_stub_table:
	dc.l	xstub_2
	dc.l	xstub_3
	dc.l	xstub_4
	dc.l	xstub_5
	dc.l	xstub_6
	dc.l	xstub_7
	dc.l	xstub_8
	dc.l	xstub_9
	dc.l	xstub_10
	dc.l	xstub_11
	dc.l	xstub_12
	dc.l	xstub_13
	dc.l	xstub_14
	dc.l	xstub_15
	dc.l	xstub_16
	dc.l	xstub_17
	dc.l	xstub_18
	dc.l	xstub_19
	dc.l	xstub_20
	dc.l	xstub_21
	dc.l	xstub_22
	dc.l	xstub_23
	dc.l	xstub_24
	dc.l	xstub_25
	dc.l	xstub_26
	dc.l	xstub_27
	dc.l	xstub_28
	dc.l	xstub_29
	dc.l	xstub_30
	dc.l	xstub_31
	dc.l	xstub_32
	dc.l	xstub_33
	dc.l	xstub_34
	dc.l	xstub_35
	dc.l	xstub_36
	dc.l	xstub_37
	dc.l	xstub_38
	dc.l	xstub_39
	dc.l	xstub_40
	dc.l	xstub_41
	dc.l	xstub_42
	dc.l	xstub_43
	dc.l	xstub_44
	dc.l	xstub_45
	dc.l	xstub_46
	dc.l	xstub_47
	dc.l	xstub_48
	dc.l	xstub_49
	dc.l	xstub_50
	dc.l	xstub_51
	dc.l	xstub_52
	dc.l	xstub_53
	dc.l	xstub_54
	dc.l	xstub_55
	dc.l	xstub_56
	dc.l	xstub_57
	dc.l	xstub_58
	dc.l	xstub_59
	dc.l	xstub_60
	dc.l	xstub_61
	dc.l	xstub_62
	dc.l	xstub_63
