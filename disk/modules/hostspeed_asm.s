; hostspeed module: the timed blocks (see hostspeed.c).
;
; Every block is   ULONG hs_<name>(ULONG units, void *mem)
;   units  outer loop count (>= 1); one unit = 16 DBF iterations of a body
;          of 64 unrolled instructions = 1024 instructions (512 for the
;          blocks whose body is 32 reload+instruction pairs, see hostspeed.c)
;   mem    1 KB of the module's any-RAM pool (16-byte aligned)
; The operands keep every instruction on its normal path for the whole
; block: no exception, no overflow, no denormal/NaN/infinity; where an
; instruction changes its own operand the body alternates it with its
; inverse (MULU.L a / MULS.L a^-1 mod 2^32, ABCD/SBCD, ROXL/ROXR, ...) or
; uses a fixed point of it (DIVU.W/DIVS.W, the 64-bit DIVU.L/DIVS.L).
;
; Called only for the CPU/FPU the probe found (hostspeed.c decides);
; assembled for the 68020 + 6888x instruction set.
	machine	68020
	fpu	1
	opt	o-			; exactly the instructions written

	section	.text,code

HS_IN	macro
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),d6		; units
	move.l	52(sp),a5		; mem
	endm
HS_OUT	macro
	moveq	#0,d0
	movem.l	(sp)+,d2-d7/a2-a6
	rts
	endm

	xdef	_hs_stop
	xdef	_hs_empty
	xdef	_hs_add_l
	xdef	_hs_div_w
	xdef	_hs_abcd_sbcd
	xdef	_hs_roxl_roxr
	xdef	_hs_movem
	xdef	_hs_movep
	xdef	_hs_trap_rte
	xdef	_hs_rte
	xdef	_hs_mul_l
	xdef	_hs_mul_l64
	xdef	_hs_div_l
	xdef	_hs_div_l64
	xdef	_hs_bfextu
	xdef	_hs_bfffo
	xdef	_hs_cas
	xdef	_hs_cas2
	xdef	_hs_chk2
	xdef	_hs_pack_unpk
	xdef	_hs_fpu_reset
	xdef	_hs_pcr_get
	xdef	_hs_pcr_set
	xdef	_hs_fadd
	xdef	_hs_fmul
	xdef	_hs_fdiv
	xdef	_hs_fsqrt
	xdef	_hs_fmod
	xdef	_hs_frem
	xdef	_hs_fscale
	xdef	_hs_fint
	xdef	_hs_fintrz
	xdef	_hs_fgetexp
	xdef	_hs_fgetman
	xdef	_hs_fsin
	xdef	_hs_fcos
	xdef	_hs_ftan
	xdef	_hs_fsincos
	xdef	_hs_fasin
	xdef	_hs_facos
	xdef	_hs_fatan
	xdef	_hs_fatanh
	xdef	_hs_fsinh
	xdef	_hs_fcosh
	xdef	_hs_ftanh
	xdef	_hs_fetox
	xdef	_hs_fetoxm1
	xdef	_hs_ftwotox
	xdef	_hs_ftentox
	xdef	_hs_flogn
	xdef	_hs_flognp1
	xdef	_hs_flog2
	xdef	_hs_flog10
	xdef	_hs_fmovecr
	xdef	_hs_fmove_p_in
	xdef	_hs_fmove_p_out
	xdef	_hs_fmovem
	xdef	_hs_fsave_frestore

; ------------------------------------------------------------ 68000 and up
_hs_stop:				; STOP until the next interrupt (the runtime's
	HS_IN				; VBL): one unit = one frame, the CPU idle
.o:	stop	#$2000
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_empty:				; the loop alone
	HS_IN
.o:	moveq	#15,d7
.i:	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_add_l:				; integer reference
	HS_IN
	moveq	#0,d0
	moveq	#1,d1
.o:	moveq	#15,d7
.i:
	rept	64
	add.l	d1,d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_div_w:				; 65539000 / 21846 = 3000 r 1000: d0 = $03E80BB8 again
	HS_IN
	move.l	#$03e80bb8,d0
	move.l	#21846,d1
.o:	moveq	#15,d7
.i:
	rept	32
	divu.w	d1,d0
	divs.w	d1,d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_abcd_sbcd:				; 45+27 = 72, 72-27 = 45 (X stays clear)
	HS_IN
	moveq	#$45,d0
	moveq	#$27,d1
	move.w	#0,ccr
.o:	moveq	#15,d7
.i:
	rept	32
	abcd	d1,d0
	sbcd	d1,d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_roxl_roxr:				; 33-bit rotate by 7 and back
	HS_IN
	move.l	#$12345678,d0
	moveq	#7,d1
	move.w	#0,ccr
.o:	moveq	#15,d7
.i:
	rept	32
	roxl.l	d1,d0
	roxr.l	d1,d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_movem:				; 11 longs out and back in
	HS_IN
	moveq	#1,d0
	moveq	#2,d1
	moveq	#3,d2
	moveq	#4,d3
	moveq	#5,d4
	moveq	#6,d5
	move.l	a5,a0
	move.l	a5,a1
	move.l	a5,a2
	move.l	a5,a3
	move.l	a5,a4
.o:	moveq	#15,d7
.i:
	rept	32
	movem.l	d0-d5/a0-a4,(a5)
	movem.l	(a5),d0-d5/a0-a4
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_movep:				; 68000-68040 (unimplemented on the 68060)
	HS_IN
	move.l	#$11223344,d0
.o:	moveq	#15,d7
.i:
	rept	32
	movep.l	d0,0(a5)
	movep.l	0(a5),d1
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_trap_rte:				; TRAP #0 -> vector 32 = _hs_rte (hostspeed.c)
	HS_IN
.o:	moveq	#15,d7
.i:
	rept	64
	trap	#0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_rte:
	rte

; ------------------------------------------------------------ 68020 and up
_hs_mul_l:				; 32-bit forms: x * a * a^-1 = x (mod 2^32)
	HS_IN
	move.l	#$12345678,d0
	move.l	#$2545f491,d1
	move.l	#$41444c71,d2		; $2545F491^-1 mod 2^32
.o:	moveq	#15,d7
.i:
	rept	32
	mulu.l	d1,d0
	muls.l	d2,d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_mul_l64:				; 64-bit forms (020-040; unimplemented on the 060)
	HS_IN
	move.l	#$12345678,d0
	move.l	#$2545f491,d1
	move.l	#$41444c71,d2
.o:	moveq	#15,d7
.i:
	rept	32
	mulu.l	d1,d3:d0
	muls.l	d2,d3:d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_div_l:				; 32-bit forms: dividend < divisor, so the
	HS_IN				; dividend moves between Dq and Dr and back
	move.l	#$12345678,d0
	move.l	#$7fff0001,d1
	moveq	#0,d2
.o:	moveq	#15,d7
.i:
	rept	32
	divul.l	d1,d2:d0		; d0 = 0, d2 = x
	divsl.l	d1,d0:d2		; d2 = 0, d0 = x
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_div_l64:				; 64/32: $12345678:369D0368 / $55555556
	HS_IN				; = $369D0368 r $12345678 (a fixed point)
	move.l	#$369d0368,d0
	move.l	#$55555556,d1
	move.l	#$12345678,d2
.o:	moveq	#15,d7
.i:
	rept	32
	divu.l	d1,d2:d0
	divs.l	d1,d2:d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_bfextu:				; memory bit field, dynamic offset/width
	HS_IN
	move.l	#$a5c3e187,(a5)
	move.l	#$12345678,4(a5)
	moveq	#5,d2
	moveq	#19,d3
.o:	moveq	#15,d7
.i:
	rept	64
	bfextu	(a5){d2:d3},d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_bfffo:
	HS_IN
	move.l	#$00012345,(a5)
	move.l	#$12345678,4(a5)
	moveq	#5,d2
	moveq	#19,d3
.o:	moveq	#15,d7
.i:
	rept	64
	bfffo	(a5){d2:d3},d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_cas:				; compare equal, writes the same value back
	HS_IN
	move.l	#$5a5a1234,d0
	move.l	d0,d1
	move.l	d0,(a5)
.o:	moveq	#15,d7
.i:
	rept	64
	cas.l	d0,d1,(a5)
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_cas2:				; 020-040 (unimplemented on the 060)
	HS_IN
	move.l	#$5a5a1234,d0
	move.l	#$0f0f4321,d1
	move.l	d0,d2
	move.l	d1,d3
	move.l	a5,a3
	lea	8(a5),a4
	move.l	d0,(a3)
	move.l	d1,(a4)
.o:	moveq	#15,d7
.i:
	rept	64
	cas2.l	d0:d1,d2:d3,(a3):(a4)
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_chk2:				; 0 <= 500 <= 1000: in bounds, no trap
	HS_IN
	clr.l	(a5)
	move.l	#1000,4(a5)
	move.l	#500,d0
.o:	moveq	#15,d7
.i:
	rept	64
	chk2.l	(a5),d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_pack_unpk:				; register forms, sources unchanged
	HS_IN
	move.l	#$00000305,d0
	moveq	#$35,d1
.o:	moveq	#15,d7
.i:
	rept	32
	pack	d0,d1,#0
	unpk	d1,d2,#$3030
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

; ------------------------------------------------------------ 68060 PCR
_hs_pcr_get:
	dc.w	$4e7a,$0808		; movec pcr,d0
	rts
_hs_pcr_set:
	move.l	4(sp),d0
	dc.w	$4e7b,$0808		; movec d0,pcr
	rts

; ------------------------------------------------------------ FPU
_hs_fpu_reset:				; FRESTORE of a null frame: FPU reset
	frestore nullframe(pc)
	rts

; fp0 1.5   fp1 0.5 (monadic source)   fp2 2.0   fp3 7.25   fp4 3.0
; fp5 -3.0  fp6 1234.5678              fp7 1/3   FPCR 0 (nearest, extended)
hs_fset:
	fmove.l	#0,fpcr
	fmove.d	c_1p5(pc),fp0
	fmove.d	c_0p5(pc),fp1
	fmove.d	c_2(pc),fp2
	fmove.d	c_7p25(pc),fp3
	fmove.d	c_3(pc),fp4
	fmove.d	c_m3(pc),fp5
	fmove.d	c_1234(pc),fp6
	fmove.d	c_third(pc),fp7
	fmove.l	#0,fpsr
	rts

; one FPU block: \1 = name, \2 = the instruction (64 copies)
HSF1	macro
_hs_\1:
	HS_IN
	bsr	hs_fset
.o:	moveq	#15,d7
.i:
	rept	64
	\2
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT
	endm

; two instructions alternating (32 pairs)
HSF2	macro
_hs_\1:
	HS_IN
	bsr	hs_fset
.o:	moveq	#15,d7
.i:
	rept	32
	\2
	\3
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT
	endm

	HSF1	fadd,<fadd.x fp1,fp0>				; FPU reference: 1.5 + n*0.5, exact
	HSF2	fmul,<fmul.x fp4,fp0>,<fmul.x fp7,fp0>
	HSF2	fdiv,<fdiv.x fp4,fp0>,<fdiv.x fp7,fp0>
	HSF1	fsqrt,<fsqrt.x fp1,fp0>
	HSF2	fmod,<fmove.x fp3,fp0>,<fmod.x fp2,fp0>		; 7.25 mod 2 = 1.25
	HSF2	frem,<fmove.x fp3,fp0>,<frem.x fp2,fp0>		; 7.25 rem 2 = -0.75
	HSF2	fscale,<fscale.x fp4,fp0>,<fscale.x fp5,fp0>
	HSF1	fint,<fint.x fp6,fp0>
	HSF1	fintrz,<fintrz.x fp6,fp0>
	HSF1	fgetexp,<fgetexp.x fp6,fp0>
	HSF1	fgetman,<fgetman.x fp6,fp0>
	HSF1	fsin,<fsin.x fp1,fp0>
	HSF1	fcos,<fcos.x fp1,fp0>
	HSF1	ftan,<ftan.x fp1,fp0>
	HSF1	fsincos,<fsincos.x fp1,fp2:fp0>
	HSF1	fasin,<fasin.x fp1,fp0>
	HSF1	facos,<facos.x fp1,fp0>
	HSF1	fatan,<fatan.x fp1,fp0>
	HSF1	fatanh,<fatanh.x fp1,fp0>
	HSF1	fsinh,<fsinh.x fp1,fp0>
	HSF1	fcosh,<fcosh.x fp1,fp0>
	HSF1	ftanh,<ftanh.x fp1,fp0>
	HSF1	fetox,<fetox.x fp1,fp0>
	HSF1	fetoxm1,<fetoxm1.x fp1,fp0>
	HSF1	ftwotox,<ftwotox.x fp1,fp0>
	HSF1	ftentox,<ftentox.x fp1,fp0>
	HSF1	flogn,<flogn.x fp1,fp0>
	HSF1	flognp1,<flognp1.x fp1,fp0>
	HSF1	flog2,<flog2.x fp1,fp0>
	HSF1	flog10,<flog10.x fp1,fp0>
	HSF1	fmovecr,<fmovecr.x #0,fp0>			; pi

_hs_fmove_p_in:				; +1.2345678E+03 packed decimal
	HS_IN
	bsr	hs_fset
	move.l	#$00030001,(a5)
	move.l	#$23456780,4(a5)
	clr.l	8(a5)
.o:	moveq	#15,d7
.i:
	rept	64
	fmove.p	(a5),fp0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_fmove_p_out:			; 1234.5678 to packed, k-factor 6
	HS_IN
	bsr	hs_fset
.o:	moveq	#15,d7
.i:
	rept	64
	fmove.p	fp6,16(a5){#6}
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_fmovem:				; 8 registers out and back in
	HS_IN
	bsr	hs_fset
	lea	64(a5),a3
.o:	moveq	#15,d7
.i:
	rept	32
	fmovem.x fp0-fp7,(a3)
	fmovem.x (a3),fp0-fp7
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

_hs_fsave_frestore:			; idle frame out and back in
	HS_IN
	bsr	hs_fset
	lea	512(a5),a4
.o:	moveq	#15,d7
.i:
	rept	32
	fsave	(a4)
	frestore (a4)
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	HS_OUT

	cnop	0,4
nullframe:	dc.l	0,0,0
c_1p5:		dc.d	1.5
c_0p5:		dc.d	0.5
c_2:		dc.d	2.0
c_7p25:		dc.d	7.25
c_3:		dc.d	3.0
c_m3:		dc.d	-3.0
c_1234:		dc.d	1234.5678
c_third:	dc.d	0.33333333333333333
