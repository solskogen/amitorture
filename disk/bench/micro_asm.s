; bench micro tier: one instruction class per block (micro.c).
;
; Every block is   ULONG bm_<name>(ULONG units, void *mem)
;   units  outer loop count (>= 1); one unit = 16 DBF iterations of a body
;          of 64 unrolled instructions = 1024 instructions (512 for the
;          blocks whose body is 32 reload+instruction pairs, see micro.c)
;   mem    the block's memory (16-byte aligned)
; and returns a checksum of the registers it worked on (D0-D5, and FP0-FP3
; rounded to single for the FPU blocks), so that wrong or missing work
; shows. The operands keep every instruction on its normal path for the
; whole block: no exception, no overflow, no denormal/NaN/infinity; where an
; instruction changes its own operand the body alternates it with its
; inverse (MULU.L a / MULS.L a^-1 mod 2^32, ABCD/SBCD, ROXL/ROXR, ...) or
; uses a fixed point of it (DIVU.W/DIVS.W, the 64-bit DIVU.L/DIVS.L).
;
; Called only for the CPU/FPU the probe found (micro.c decides);
; assembled for the 68020 + 6888x instruction set. Ported from the
; accuracy disks' former hostspeed module.
	machine	68020
	fpu	1
	opt	o-			; exactly the instructions written

	section	.text,code

BM_IN	macro
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),d6		; units
	move.l	52(sp),a5		; mem
	moveq	#0,d0
	moveq	#0,d1
	moveq	#0,d2
	moveq	#0,d3
	moveq	#0,d4
	moveq	#0,d5
	endm
BM_OUT	macro				; checksum: D0 + D1 + ... + D5
	add.l	d1,d0
	add.l	d2,d0
	add.l	d3,d0
	add.l	d4,d0
	add.l	d5,d0
	movem.l	(sp)+,d2-d7/a2-a6
	rts
	endm
BMF_OUT	macro				; FP0-FP3 as singles, then as BM_OUT
	fmove.s	fp0,-(sp)
	add.l	(sp)+,d0
	fmove.s	fp1,-(sp)
	add.l	(sp)+,d0
	fmove.s	fp2,-(sp)
	add.l	(sp)+,d0
	fmove.s	fp3,-(sp)
	add.l	(sp)+,d0
	BM_OUT
	endm

	xdef	_bm_stop
	xdef	_bm_mem_read
	xdef	_bm_vbl_poll
	xdef	_bm_empty
	xdef	_bm_add_l
	xdef	_bm_div_w
	xdef	_bm_abcd_sbcd
	xdef	_bm_roxl_roxr
	xdef	_bm_movem
	xdef	_bm_movep
	xdef	_bm_trap_rte
	xdef	_bm_rte
	xdef	_bm_mul_l
	xdef	_bm_mul_l64
	xdef	_bm_div_l
	xdef	_bm_div_l64
	xdef	_bm_bfextu
	xdef	_bm_bfffo
	xdef	_bm_cas
	xdef	_bm_cas2
	xdef	_bm_chk2
	xdef	_bm_pack_unpk
	xdef	_bm_custom_read
	xdef	_bm_cia_read
	xdef	_bm_cia_write
	xdef	_bm_movec_vbr
	xdef	_bm_pmove_tc
	xdef	_bm_move16
	xdef	_bm_movec_pcr
	xdef	_bm_fadd
	xdef	_bm_fmul
	xdef	_bm_fdiv
	xdef	_bm_fsqrt
	xdef	_bm_frem
	xdef	_bm_fscale
	xdef	_bm_fintrz
	xdef	_bm_fgetexp
	xdef	_bm_fsin
	xdef	_bm_fatan
	xdef	_bm_fetox
	xdef	_bm_flogn
	xdef	_bm_fmovecr
	xdef	_bm_fmove_p_in
	xdef	_bm_fmove_p_out
	xdef	_bm_fmovem
	xdef	_bm_fsave_frestore

; ------------------------------------------------------------ 68000 and up
_bm_stop:				; STOP until the next interrupt (the runtime's
	BM_IN				; VBL): one unit = one frame, the CPU idle
.o:	stop	#$2000
	subq.l	#1,d6
	bne	.o
	BM_OUT

_bm_vbl_poll:				; poll the beam until line $2C starts, then
	BM_IN				; until it ends: one unit = one frame, the
	lea	$dff004,a0		; CPU busy-waiting as games do
	move.l	#$1ff00,d2		; VPOSR V8 and VHPOSR V7-V0
	move.l	#$2c00,d3
.o:	move.l	(a0),d0
	and.l	d2,d0
	cmp.l	d3,d0
	bne	.o
.n:	move.l	(a0),d0
	and.l	d2,d0
	cmp.l	d3,d0
	beq	.n
	subq.l	#1,d6
	bne	.o
	BM_OUT

_bm_empty:				; the loop alone
	BM_IN
.o:	moveq	#15,d7
.i:	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BM_OUT

_bm_add_l:				; integer reference
	BM_IN
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
	BM_OUT

_bm_div_w:				; 65539000 / 21846 = 3000 r 1000: d0 = $03E80BB8 again
	BM_IN
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
	BM_OUT

_bm_abcd_sbcd:				; 45+27 = 72, 72-27 = 45 (X stays clear)
	BM_IN
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
	BM_OUT

_bm_roxl_roxr:				; 33-bit rotate by 7 and back
	BM_IN
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
	BM_OUT

_bm_movem:				; 11 longs out and back in
	BM_IN
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
	BM_OUT

_bm_movep:				; 68000-68040 (unimplemented on the 68060)
	BM_IN
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
	BM_OUT

_bm_trap_rte:				; TRAP #0 -> vector 32 = _bm_rte
	BM_IN
.o:	moveq	#15,d7
.i:
	rept	64
	trap	#0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BM_OUT

_bm_rte:
	rte

; Read-only hardware registers: neither VHPOSR nor CIA-A PRA acknowledges IRQs.
_bm_custom_read:
	BM_IN
	lea	$dff006,a0
.o:	moveq	#15,d7
.i:
	rept	64
	move.w	(a0),d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BM_OUT

_bm_cia_read:
	BM_IN
	lea	$bfe001,a0
.o:	moveq	#15,d7
.i:
	rept	64
	move.b	(a0),d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BM_OUT

_bm_cia_write:				; BSET/BCLR the power LED bit (CIA-A PRA
	BM_IN				; bit 1), as Kickstart's Alert blinks the LED:
	lea	$bfe001,a0		; back-to-back CIA writes. The caller puts
.o:	moveq	#15,d7			; the bit back afterwards.
.i:
	rept	32
	bset	#1,(a0)
	bclr	#1,(a0)
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BM_OUT

; 68010+: read VBR, without changing exception vectors.
_bm_movec_vbr:
	BM_IN
.o:	moveq	#15,d7
.i:
	rept	64
	movec	vbr,d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BM_OUT

; 68030 MMU only: PMOVE TC to our pool, never change translation state.
_bm_pmove_tc:
	BM_IN
.o:	moveq	#15,d7
.i:
	rept	64
	dc.w	$f015,$4200		; pmove tc,(a5)
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BM_OUT

; 68040/68060: 64 aligned 16-byte copies per inner loop, within our pool.
	machine	68040
_bm_move16:
	BM_IN
.o:	moveq	#15,d7
.i:	move.l	a5,a0
	lea	1024(a5),a1
	rept	64
	move16	(a0)+,(a1)+
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BM_OUT
	machine	68020

; 68060 only: PCR read, preserving FPU enable and superscalar controls.
_bm_movec_pcr:
	BM_IN
.o:	moveq	#15,d7
.i:
	rept	64
	dc.w	$4e7a,$0808		; movec pcr,d0
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BM_OUT

;--- memory read bandwidth (xsysinfo's method): 64 KB per unit in bursts of
; four MOVEM.L of 8 longs (128 bytes per iteration), from mem (16-byte
; aligned). The checksum is the last 32 bytes read.
_bm_mem_read:
	BM_IN
.o:	move.l	a5,a0
	move.w	#511,d7
.i:
	rept	4
	movem.l	(a0)+,d0-d5/a1-a2
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	add.l	a1,d0
	add.l	a2,d0
	BM_OUT

; ------------------------------------------------------------ 68020 and up
_bm_mul_l:				; 32-bit forms: x * a * a^-1 = x (mod 2^32)
	BM_IN
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
	BM_OUT

_bm_mul_l64:				; 64-bit forms (020-040; unimplemented on the 060)
	BM_IN
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
	BM_OUT

_bm_div_l:				; 32-bit forms: dividend < divisor, so the
	BM_IN				; dividend moves between Dq and Dr and back
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
	BM_OUT

_bm_div_l64:				; 64/32: $12345678:369D0368 / $55555556
	BM_IN				; = $369D0368 r $12345678 (a fixed point)
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
	BM_OUT

_bm_bfextu:				; memory bit field, dynamic offset/width
	BM_IN
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
	BM_OUT

_bm_bfffo:
	BM_IN
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
	BM_OUT

_bm_cas:				; compare equal, writes the same value back
	BM_IN
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
	BM_OUT

_bm_cas2:				; 020-040 (unimplemented on the 060)
	BM_IN
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
	BM_OUT

_bm_chk2:				; 0 <= 500 <= 1000: in bounds, no trap
	BM_IN
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
	BM_OUT

_bm_pack_unpk:				; register forms, sources unchanged
	BM_IN
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
	BM_OUT

; fp0 1.5   fp1 0.5 (monadic source)   fp2 2.0   fp3 7.25   fp4 3.0
; fp5 -3.0  fp6 1234.5678              fp7 1/3   FPCR 0 (nearest, extended)
bm_fset:
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
BMF1	macro
_bm_\1:
	BM_IN
	bsr	bm_fset
.o:	moveq	#15,d7
.i:
	rept	64
	\2
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BMF_OUT
	endm

; two instructions alternating (32 pairs)
BMF2	macro
_bm_\1:
	BM_IN
	bsr	bm_fset
.o:	moveq	#15,d7
.i:
	rept	32
	\2
	\3
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BMF_OUT
	endm

	BMF1	fadd,<fadd.x fp1,fp0>				; FPU reference: 1.5 + n*0.5, exact
	BMF2	fmul,<fmul.x fp4,fp0>,<fmul.x fp7,fp0>
	BMF2	fdiv,<fdiv.x fp4,fp0>,<fdiv.x fp7,fp0>
	BMF1	fsqrt,<fsqrt.x fp1,fp0>
	BMF2	frem,<fmove.x fp3,fp0>,<frem.x fp2,fp0>		; 7.25 rem 2 = -0.75
	BMF2	fscale,<fscale.x fp4,fp0>,<fscale.x fp5,fp0>
	BMF1	fintrz,<fintrz.x fp6,fp0>
	BMF1	fgetexp,<fgetexp.x fp6,fp0>
	BMF1	fsin,<fsin.x fp1,fp0>
	BMF1	fatan,<fatan.x fp1,fp0>
	BMF1	fetox,<fetox.x fp1,fp0>
	BMF1	flogn,<flogn.x fp1,fp0>
	BMF1	fmovecr,<fmovecr.x #0,fp0>			; pi

_bm_fmove_p_in:				; +1.2345678E+03 packed decimal
	BM_IN
	bsr	bm_fset
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
	BMF_OUT

_bm_fmove_p_out:			; 1234.5678 to packed, k-factor 6
	BM_IN
	bsr	bm_fset
.o:	moveq	#15,d7
.i:
	rept	64
	fmove.p	fp6,16(a5){#6}
	endr
	dbf	d7,.i
	subq.l	#1,d6
	bne	.o
	BMF_OUT

_bm_fmovem:				; 8 registers out and back in
	BM_IN
	bsr	bm_fset
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
	BMF_OUT

_bm_fsave_frestore:			; idle frame out and back in
	BM_IN
	bsr	bm_fset
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
	BMF_OUT

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
