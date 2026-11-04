; fpu module: hand-written helpers (see fpu.c). Called only when the probe
; found an FPU. gcc ABI: arguments on the stack, d0-d1/a0-a1 scratch.
	machine	68040
	fpu	1

	xdef	_fpu_exec
	xdef	_fpu_pred
	xdef	_fpu_pred_fpsr
	xdef	_fpu_ftrap_handler
	xdef	_fpu_pcr_get
	xdef	_fpu_pcr_set
	xdef	_fpu_frestore_null_test
	xdef	_fpu_frames
	xdef	_fpu_frestore_bad
	xdef	_fpu_bad_frame
	xdef	_fpu_fmovem_static
	xdef	_fpu_fmovem_predec
	xdef	_fpu_dyn_run
	xdef	_fpu_dyn_buf
	xdef	_fpu_ctl_rw
	xdef	_fpu_reset
	xdef	_fpu_after_trap
	xdef	_fpu_trap_run
	xdef	_fpu_trap_tab
	xdef	_fpu_n_trap
	xdef	_fpu_trap_idx
	xdef	_fpu_exc_run
	xdef	_fpu_exc_tab
	xdef	_fpu_n_exc
	xdef	_fpu_exc_idx

	xref	_fpu_optab
	xref	_fpu_predtab
	xref	_fpu_trapcnt

	section	.text,code

; ---------------------------------------------------------------- vectors
; void fpu_exec(const struct fvec *v, struct fout *o)
;   fvec: op.w flags.w fpcr.l src[3] dst[3] ...; fout: res[3] fpsr res2[3]
_fpu_exec:
	movem.l	d2/a2,-(sp)
	move.l	12(sp),a0
	move.l	16(sp),a1
	moveq	#0,d0
	move.l	d0,(a1)
	move.l	d0,4(a1)
	move.l	d0,8(a1)
	move.l	d0,12(a1)
	move.l	d0,16(a1)
	move.l	d0,20(a1)
	move.l	d0,24(a1)
	fmove.l	d0,fpcr
	fmovem.x 8(a0),fp1		; raw loads: no conversion, no traps
	fmovem.x 20(a0),fp0
	fmovem.x zero12,fp2
	lea	8(a0),a2		; source memory operand
	move.l	4(a0),d2		; FPCR for the operation
	move.w	(a0),d0
	lsl.l	#2,d0
	lea	_fpu_optab,a0
	move.l	(a0,d0.l),a0
	fmove.l	#0,fpsr
	fmove.l	d2,fpcr
	jsr	(a0)			; d1 = 1: result in fp0
	fmove.l	fpsr,d0
	fmove.l	#0,fpcr
	move.l	d0,12(a1)
	tst.w	d1
	beq.s	.mem
	fmovem.x fp0,(a1)
.mem:	fmovem.x fp2,16(a1)
	fmove.l	#0,fpsr
	movem.l	(sp)+,d2/a2
	rts

; ---------------------------------------------------------------- predicates
; ULONG fpu_pred(ULONG idx, ULONG fpsr_in): idx = family*32 + predicate
; (0 FBcc, 1 FScc, 2 FDBcc, 3 FTRAPcc). Returns the outcome; FPSR after in
; fpu_pred_fpsr.
_fpu_pred:
	move.l	d2,-(sp)
	move.l	8(sp),d0
	lsl.l	#2,d0
	lea	_fpu_predtab,a0
	move.l	(a0,d0.l),a0
	move.l	12(sp),d0
	fmove.l	#0,fpcr
	jsr	(a0)
	move.l	d1,_fpu_pred_fpsr
	fmove.l	#0,fpsr
	move.l	(sp)+,d2
	rts

_fpu_ftrap_handler:			; vector 7 (FTRAPcc taken)
	addq.l	#1,_fpu_trapcnt
	rte

; ---------------------------------------------------------------- 68060 PCR
_fpu_pcr_get:
	dc.w	$4e7a,$0808		; movec pcr,d0
	rts
_fpu_pcr_set:
	move.l	4(sp),d0
	dc.w	$4e7b,$0808		; movec d0,pcr
	rts

; ---------------------------------------------------------------- reset
_fpu_reset:				; FRESTORE a null frame: FPU reset
	frestore nullframe
	rts

; ---------------------------------------------------------------- FRESTORE null
; void fpu_frestore_null_test(ULONG out[3+24])
;   out[0..2] = FPCR FPSR FPIAR after the reset, out[3..26] = FP0-FP7
_fpu_frestore_null_test:
	move.l	4(sp),a0
	fmove.l	#$00000030,fpcr		; RP, no enables
	fmove.l	#$0f00fff8,fpsr
	fmove.l	#$12345678,fpiar
	fmove.l	#1,fp0
	fmove.l	#2,fp1
	fmove.l	#3,fp2
	fmove.l	#4,fp3
	fmove.l	#5,fp4
	fmove.l	#6,fp5
	fmove.l	#7,fp6
	fmove.l	#8,fp7
	frestore nullframe
	fmovem.l fpcr/fpsr/fpiar,(a0)
	fmovem.x fp0-fp7,12(a0)
	rts

; ---------------------------------------------------------------- FSAVE frames
; void fpu_frames(ULONG out[32])
;   out[0]  bytes FSAVE -(An) wrote after a null FRESTORE; out[1..3] frame
;   out[4]  bytes after one FP instruction (idle);           out[5..7] frame
;   out[8]  FSAVE idle (An) / FRESTORE (An) / FSAVE round trip: frame1 long 0
;   out[9]  second frame long 0;  out[10] FPCR out[11] FPSR out[12] FPIAR
;   out[13..15] FP3 after the round trip
_fpu_frames:
	movem.l	d2/a2,-(sp)
	move.l	12(sp),a1
	frestore nullframe
	lea	fbuf1_end,a0
	move.l	a0,d2
	fsave	-(a0)
	sub.l	a0,d2
	move.l	d2,(a1)
	move.l	(a0),4(a1)
	move.l	4(a0),8(a1)
	move.l	8(a0),12(a1)
	fmove.l	#1,fp0			; any FP instruction: null -> idle
	fnop				; wait for it to finish: a 68882 still
					; executing would save a busy frame
	lea	fbuf1_end,a0
	move.l	a0,d2
	fsave	-(a0)
	sub.l	a0,d2
	move.l	d2,16(a1)
	move.l	(a0),20(a1)
	move.l	4(a0),24(a1)
	move.l	8(a0),28(a1)
	; round trip through an idle frame
	fmove.l	#$00000020,fpcr		; RM
	fmove.x	fp0,fp3
	fdiv.l	#3,fp3			; FP3 = 1/3 rounded toward minus infinity
	fmove.l	#$08000000,fpsr		; N
	fmove.l	#$00c0ffee,fpiar
	fnop				; idle (not busy) frame for the round trip
	lea	fbuf1,a0
	fsave	(a0)
	frestore (a0)
	lea	fbuf2,a2
	fsave	(a2)
	move.l	(a0),32(a1)
	move.l	(a2),36(a1)
	frestore (a2)
	fmovem.l fpcr/fpsr/fpiar,40(a1)
	fmovem.x fp3,52(a1)
	fmove.l	#0,fpcr
	fmove.l	#0,fpsr
	movem.l	(sp)+,d2/a2
	rts

; FRESTORE of an invalid frame (run under amt_try): format error, vector 14
_fpu_frestore_bad:
	lea	_fpu_bad_frame,a0
	frestore (a0)
	rts

; ---------------------------------------------------------------- FMOVEM.X
; void fpu_fmovem_static(const ULONG *src96, ULONG *dst96)
_fpu_fmovem_static:
	move.l	4(sp),a0
	move.l	8(sp),a1
	fmovem.x (a0),fp0-fp7
	fmovem.x fp0-fp7,(a1)
	rts
; ULONG fpu_fmovem_predec(const ULONG *src96, ULONG *dst_end): returns the
; final address register; memory gets FP0 lowest ... FP7 highest
_fpu_fmovem_predec:
	move.l	4(sp),a0
	move.l	8(sp),a1
	fmovem.x (a0)+,fp0-fp7
	fmovem.x fp0-fp7,-(a1)
	move.l	a1,d0
	rts
; dynamic register lists (run under amt_try; unimplemented EA on the 060)
; fpu_dyn_buf: [0..95] source FP0-FP7, [96..191] store area, [192] mask
; store: mask $A5 (control mode: bit 7 = FP0) -> FP0, FP2, FP5, FP7
; then load back with mask $5A from the source into FP1 FP3 FP4 FP6, and
; store everything statically to [208..303]
_fpu_dyn_run:
	lea	_fpu_dyn_buf,a0
	fmovem.x (a0),fp0-fp7
	moveq	#0,d0
	move.b	192(a0),d0
	lea	96(a0),a1
	fmovem.x d0,(a1)
	moveq	#0,d1
	move.b	193(a0),d1
	lea	(a0),a1
	fmovem.x (a1)+,d1
	lea	208(a0),a1
	fmovem.x fp0-fp7,(a1)
	rts

; ---------------------------------------------------------------- control regs
; void fpu_ctl_rw(ULONG out[3]): writes all ones to FPCR, FPSR, FPIAR and
; reads each back (FPCR is cleared again before FPSR is written)
_fpu_ctl_rw:
	move.l	4(sp),a0
	moveq	#-1,d0
	fmove.l	d0,fpcr
	fmove.l	fpcr,(a0)
	fmove.l	#0,fpcr
	fmove.l	d0,fpsr
	fmove.l	fpsr,4(a0)
	fmove.l	#0,fpsr
	fmove.l	d0,fpiar
	fmove.l	fpiar,8(a0)
	fmove.l	#0,fpiar
	rts

; ---------------------------------------------------------------- after a trap
; void fpu_after_trap(ULONG out[8]): FSAVE first (takes the FPU out of its
; exception state), then FPSR, FPIAR; then a null FRESTORE resets the FPU.
;   out[0] = bytes written by FSAVE -(An), out[1..3] = frame longs 0..2,
;   out[4] = FPSR, out[5] = FPIAR
_fpu_after_trap:
	move.l	d2,-(sp)
	move.l	8(sp),a1
	lea	fbuf1_end,a0
	move.l	a0,d2
	fsave	-(a0)
	sub.l	a0,d2
	move.l	d2,(a1)
	move.l	(a0),4(a1)
	move.l	4(a0),8(a1)
	move.l	8(a0),12(a1)
	fmove.l	fpsr,16(a1)
	fmove.l	fpiar,20(a1)
	frestore nullframe
	move.l	(sp)+,d2
	rts

; ---------------------------------------------------------------- trap table
; entry (20 bytes): start, next, name, data, vector.w, fpumask.w
TENT	macro	; name, vector, mask, data
	dc.l	t_\1, t_\1_e, tn_\1, \4
	dc.w	\2, \3
	endm

; void fpu_trap_run(void) (under amt_try): entry fpu_trap_idx, with
; FP0-FP2 = 1.0, D0 = $80, A0 = the entry's data
_fpu_trap_run:
	move.l	_fpu_trap_idx,d0
	mulu.w	#20,d0
	lea	_fpu_trap_tab,a1
	add.l	d0,a1
	fmove.l	#0,fpcr
	fmove.l	#1,fp0
	fmove.l	#1,fp1
	fmove.l	#1,fp2
	fmove.l	#0,fpsr
	move.l	12(a1),a0
	move.l	(a1),a1
	move.l	#$80,d0
	jmp	(a1)

t_fsin:		fsin.x	fp1,fp0
t_fsin_e:	rts
t_fcos:		fcos.x	fp1,fp0
t_fcos_e:	rts
t_ftan:		ftan.x	fp1,fp0
t_ftan_e:	rts
t_fasin:	fasin.x	fp1,fp0
t_fasin_e:	rts
t_facos:	facos.x	fp1,fp0
t_facos_e:	rts
t_fatan:	fatan.x	fp1,fp0
t_fatan_e:	rts
t_fsinh:	fsinh.x	fp1,fp0
t_fsinh_e:	rts
t_fcosh:	fcosh.x	fp1,fp0
t_fcosh_e:	rts
t_ftanh:	ftanh.x	fp1,fp0
t_ftanh_e:	rts
t_fatanh:	fatanh.x fp1,fp0
t_fatanh_e:	rts
t_fetox:	fetox.x	fp1,fp0
t_fetox_e:	rts
t_fetoxm1:	fetoxm1.x fp1,fp0
t_fetoxm1_e:	rts
t_ftwotox:	ftwotox.x fp1,fp0
t_ftwotox_e:	rts
t_ftentox:	ftentox.x fp1,fp0
t_ftentox_e:	rts
t_flogn:	flogn.x	fp1,fp0
t_flogn_e:	rts
t_flognp1:	flognp1.x fp1,fp0
t_flognp1_e:	rts
t_flog2:	flog2.x	fp1,fp0
t_flog2_e:	rts
t_flog10:	flog10.x fp1,fp0
t_flog10_e:	rts
t_fsincos:	fsincos.x fp1,fp2:fp0
t_fsincos_e:	rts
t_fgetexp:	fgetexp.x fp1,fp0
t_fgetexp_e:	rts
t_fgetman:	fgetman.x fp1,fp0
t_fgetman_e:	rts
t_fscale:	fscale.x fp1,fp0
t_fscale_e:	rts
t_fmod:		fmod.x	fp1,fp0
t_fmod_e:	rts
t_frem:		frem.x	fp1,fp0
t_frem_e:	rts
t_fint:		fint.x	fp1,fp0
t_fint_e:	rts
t_fintrz:	fintrz.x fp1,fp0
t_fintrz_e:	rts
t_fmovecr:	fmovecr	#0,fp0
t_fmovecr_e:	rts
t_fsin_mem:	fsin.x	(a0),fp0		; memory source: EA in the frame
t_fsin_mem_e:	rts
t_fscc:		dc.w	$f240,$0001		; fseq d0
t_fscc_e:	rts
t_fdbcc:	dc.w	$f248,$0001,2		; fdbeq d0,*+4
t_fdbcc_e:	rts
t_ftrapcc:	dc.w	$f27c,$0001		; ftrapeq (FP0=1: not taken)
t_ftrapcc_e:	rts
t_fmovem_dyn:	fmovem.x d0,(a0)		; dynamic list
t_fmovem_dyn_e:	rts
t_fmove_imm_x:	dc.w	$f23c,$4800,$3fff,0,$8000,0,0,0	; fmove.x #1.0,fp0
t_fmove_imm_x_e: rts
t_fmovem_l_imm:	dc.w	$f23c,$9800,0,0,0,0	; fmovem.l #0,#0,fpcr/fpsr
t_fmovem_l_imm_e: rts
t_packed_in:	fmove.p	(a0),fp0
t_packed_in_e:	rts
t_packed_out:	fmove.p	fp0,(a0){#3}
t_packed_out_e:	rts
t_denorm_x:	fadd.x	(a0),fp0
t_denorm_x_e:	rts
t_denorm_s:	fmove.s	(a0),fp0
t_denorm_s_e:	rts
t_unnorm_x:	fmove.x	(a0),fp0
t_unnorm_x_e:	rts
t_fsadd:	fsadd.x	fp1,fp0
t_fsadd_e:	rts
t_fdmul:	fdmul.x	fp1,fp0
t_fdmul_e:	rts
t_fssqrt:	fssqrt.x fp1,fp0
t_fssqrt_e:	rts

; ---------------------------------------------------------------- exception table
; entry (24 bytes): start, fnop, name, data(src X, dst X), vector.w,
; FPCR enable.w, FPSR exception bit.w, fpumask.w
XENT	macro	; name, vector, enable, excbit, mask, data
	dc.l	x_\1, x_\1_e, xn_\1, \6
	dc.w	\2, \3, \4, \5
	endm

; void fpu_exc_run(void) (under amt_try): entry fpu_exc_idx: FP1 = src,
; FP0 = dst from data (raw), FPCR = the enable (RN, X), then the operation
; and an FNOP to bring out a deferred exception.
_fpu_exc_run:
	move.l	_fpu_exc_idx,d0
	mulu.w	#24,d0
	lea	_fpu_exc_tab,a1
	add.l	d0,a1
	fmove.l	#0,fpcr
	move.l	12(a1),a0
	fmovem.x (a0),fp1
	fmovem.x 12(a0),fp0
	lea	24(a0),a0		; extra memory operand
	moveq	#0,d0
	move.w	18(a1),d0
	fmove.l	#0,fpsr
	cmp.w	#$8000,d0		; BSUN: NAN set in the FPSR first
	bne.s	.nb
	fmove.l	#$01000000,fpsr
.nb:	fmove.l	d0,fpcr
	move.l	(a1),a1
	jmp	(a1)

x_ovfl:		fmul.x	fp1,fp0
x_ovfl_e:	fnop
		rts
x_unfl:		fmul.x	fp1,fp0
x_unfl_e:	fnop
		rts
x_dz:		fdiv.x	fp1,fp0
x_dz_e:		fnop
		rts
x_operr:	fsqrt.x	fp1,fp0
x_operr_e:	fnop
		rts
x_inex2:	fdiv.x	fp1,fp0
x_inex2_e:	fnop
		rts
x_snan:		fadd.x	fp1,fp0
x_snan_e:	fnop
		rts
x_bsun:		dc.w	$f292,2			; fbgt.w *+2 (predicate GT sets BSUN)
x_bsun_e:	fnop
		rts
x_inex1:	fmove.p	(a0),fp0
x_inex1_e:	fnop
		rts
x_ovfl_dis:	fmul.x	fp1,fp0
x_ovfl_dis_e:	fnop
		rts
x_unfl_dis:	fmul.x	fp1,fp0
x_unfl_dis_e:	fnop
		rts
x_operr_int:	fmove.l	fp0,d0
x_operr_int_e:	fnop
		rts

	section	.data,data
	cnop	0,4
nullframe:	dc.l	0,0,0
zero12:		dc.l	0,0,0
_fpu_bad_frame:	dc.l	$5a5a5a5a,0,0,0
		ds.b	256
_fpu_pred_fpsr:	dc.l	0
_fpu_trap_idx:	dc.l	0
_fpu_exc_idx:	dc.l	0

; data for the trap entries
d_none:		dc.l	$3fff0000,$80000000,0		; 1.0 extended
d_packed:	dc.l	$00000001,0,0			; packed 1.0
		ds.b	16
d_denx:		dc.l	$00000000,0,1			; extended denormal
d_dens:		dc.l	$00000001			; single denormal
d_unnx:		dc.l	$3fff0000,$40000000,0		; unnormal 0.5
d_scratch:	ds.b	32

M881	equ	1
M882	equ	2
M040	equ	4
M060	equ	8
M6888X	equ	3
M4060	equ	12

_fpu_n_trap:	dc.l	(_fpu_trap_tab_end-_fpu_trap_tab)/20
_fpu_trap_tab:
	TENT	fsin,11,M4060,d_none
	TENT	fcos,11,M4060,d_none
	TENT	ftan,11,M4060,d_none
	TENT	fasin,11,M4060,d_none
	TENT	facos,11,M4060,d_none
	TENT	fatan,11,M4060,d_none
	TENT	fsinh,11,M4060,d_none
	TENT	fcosh,11,M4060,d_none
	TENT	ftanh,11,M4060,d_none
	TENT	fatanh,11,M4060,d_none
	TENT	fetox,11,M4060,d_none
	TENT	fetoxm1,11,M4060,d_none
	TENT	ftwotox,11,M4060,d_none
	TENT	ftentox,11,M4060,d_none
	TENT	flogn,11,M4060,d_none
	TENT	flognp1,11,M4060,d_none
	TENT	flog2,11,M4060,d_none
	TENT	flog10,11,M4060,d_none
	TENT	fsincos,11,M4060,d_none
	TENT	fgetexp,11,M4060,d_none
	TENT	fgetman,11,M4060,d_none
	TENT	fscale,11,M4060,d_none
	TENT	fmod,11,M4060,d_none
	TENT	frem,11,M4060,d_none
	TENT	fint,11,M040,d_none
	TENT	fintrz,11,M040,d_none
	TENT	fmovecr,11,M4060,d_none
	TENT	fsin_mem,11,M4060,d_none
	TENT	fscc,11,M060,d_none
	TENT	fdbcc,11,M060,d_none
	TENT	ftrapcc,11,M060,d_none
	TENT	fmove_imm_x,60,M060,d_none
	TENT	fmovem_l_imm,60,M060,d_none
	TENT	packed_in,55,M4060,d_packed
	TENT	packed_out,55,M4060,d_scratch
	TENT	denorm_x,55,M4060,d_denx
	TENT	denorm_s,55,M4060,d_dens
	TENT	unnorm_x,55,M4060,d_unnx
	TENT	fsadd,11,M6888X,d_none
	TENT	fdmul,11,M6888X,d_none
	TENT	fssqrt,11,M6888X,d_none
_fpu_trap_tab_end:

; exception operands: src X, dst X, then a memory operand
e_ovfl:		dc.l	$40000000,$80000000,0, $7ffe0000,$ffffffff,$ffffffff	; max * 2
e_unfl:		dc.l	$00010000,$80000000,0, $00010000,$80000000,0		; tiny * tiny
e_dz:		dc.l	$00000000,0,0, $3fff0000,$80000000,0			; 1 / 0
e_operr:	dc.l	$bfff0000,$80000000,0, $3fff0000,$80000000,0		; sqrt(-1)
e_inex2:	dc.l	$40000000,$c0000000,0, $3fff0000,$80000000,0		; 1 / 3
e_snan:		dc.l	$7fff0000,$a0000000,0, $3fff0000,$80000000,0		; SNaN + 1
e_bsun:		dc.l	$3fff0000,$80000000,0, $3fff0000,$80000000,0
e_inex1:	dc.l	$3fff0000,$80000000,0, $3fff0000,$80000000,0
		dc.l	$40010001,0,0						; packed 1E-1
e_operr_int:	dc.l	$3fff0000,$80000000,0, $7fff0000,0,0			; (long) +inf

_fpu_n_exc:	dc.l	(_fpu_exc_tab_end-_fpu_exc_tab)/24
_fpu_exc_tab:
	XENT	ovfl,53,$1000,$1000,15,e_ovfl
	XENT	unfl,51,$0800,$0800,15,e_unfl
	XENT	dz,50,$0400,$0400,15,e_dz
	XENT	operr,52,$2000,$2000,15,e_operr
	XENT	inex2,49,$0200,$0200,15,e_inex2
	XENT	snan,54,$4000,$4000,15,e_snan
	XENT	bsun,48,$8000,$8000,15,e_bsun
	XENT	inex1,49,$0100,$0100,3,e_inex1
	XENT	operr_int,52,$2000,$2000,3,e_operr_int
	XENT	ovfl_dis,53,$0000,$1000,12,e_ovfl
	XENT	unfl_dis,51,$0000,$0800,12,e_unfl
_fpu_exc_tab_end:

tn_fsin:	dc.b	"unimp_fsin",0
tn_fcos:	dc.b	"unimp_fcos",0
tn_ftan:	dc.b	"unimp_ftan",0
tn_fasin:	dc.b	"unimp_fasin",0
tn_facos:	dc.b	"unimp_facos",0
tn_fatan:	dc.b	"unimp_fatan",0
tn_fsinh:	dc.b	"unimp_fsinh",0
tn_fcosh:	dc.b	"unimp_fcosh",0
tn_ftanh:	dc.b	"unimp_ftanh",0
tn_fatanh:	dc.b	"unimp_fatanh",0
tn_fetox:	dc.b	"unimp_fetox",0
tn_fetoxm1:	dc.b	"unimp_fetoxm1",0
tn_ftwotox:	dc.b	"unimp_ftwotox",0
tn_ftentox:	dc.b	"unimp_ftentox",0
tn_flogn:	dc.b	"unimp_flogn",0
tn_flognp1:	dc.b	"unimp_flognp1",0
tn_flog2:	dc.b	"unimp_flog2",0
tn_flog10:	dc.b	"unimp_flog10",0
tn_fsincos:	dc.b	"unimp_fsincos",0
tn_fgetexp:	dc.b	"unimp_fgetexp",0
tn_fgetman:	dc.b	"unimp_fgetman",0
tn_fscale:	dc.b	"unimp_fscale",0
tn_fmod:	dc.b	"unimp_fmod",0
tn_frem:	dc.b	"unimp_frem",0
tn_fint:	dc.b	"unimp_fint",0
tn_fintrz:	dc.b	"unimp_fintrz",0
tn_fmovecr:	dc.b	"unimp_fmovecr",0
tn_fsin_mem:	dc.b	"unimp_fsin_mem",0
tn_fscc:	dc.b	"unimp_fscc_060",0
tn_fdbcc:	dc.b	"unimp_fdbcc_060",0
tn_ftrapcc:	dc.b	"unimp_ftrapcc_060",0
tn_fmovem_dyn:	dc.b	"effadd_fmovem_dynamic_060",0
tn_fmove_imm_x:	dc.b	"effadd_fmove_imm_x_060",0
tn_fmovem_l_imm: dc.b	"effadd_fmovem_l_imm_060",0
tn_packed_in:	dc.b	"unsupp_packed_in",0
tn_packed_out:	dc.b	"unsupp_packed_out",0
tn_denorm_x:	dc.b	"unsupp_denormal_x",0
tn_denorm_s:	dc.b	"unsupp_denormal_s",0
tn_unnorm_x:	dc.b	"unsupp_unnormal_x",0
tn_fsadd:	dc.b	"fline_fsadd_6888x",0
tn_fdmul:	dc.b	"fline_fdmul_6888x",0
tn_fssqrt:	dc.b	"fline_fssqrt_6888x",0
xn_ovfl:	dc.b	"exc_ovfl",0
xn_unfl:	dc.b	"exc_unfl",0
xn_dz:		dc.b	"exc_dz",0
xn_operr:	dc.b	"exc_operr",0
xn_inex2:	dc.b	"exc_inex2",0
xn_snan:	dc.b	"exc_snan",0
xn_bsun:	dc.b	"exc_bsun",0
xn_inex1:	dc.b	"exc_inex1",0
xn_operr_int:	dc.b	"exc_operr_int",0
xn_ovfl_dis:	dc.b	"exc_ovfl_disabled_4060",0
xn_unfl_dis:	dc.b	"exc_unfl_disabled_4060",0

	section	.bss,bss
	cnop	0,4
fbuf1:		ds.b	256
fbuf1_end:
fbuf2:		ds.b	256
_fpu_dyn_buf:	ds.b	320
