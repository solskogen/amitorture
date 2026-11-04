; flags module: condition codes as consumers see them (68000 ISA only).
;
; Every routine is called from C (stack arguments, d2-d7/a2-a6 preserved).
; After "movem.l d2-d7/a2-a6,-(sp)" (44 bytes) the first argument is at 48(sp).
; Expectations are computed in flags.c from the M68000 PRM rules; nothing
; here decides pass/fail.

	section	.text,code

SAVE	macro
	movem.l	d2-d7/a2-a6,-(sp)
	endm
REST	macro
	movem.l	(sp)+,d2-d7/a2-a6
	endm

; ------------------------------------------------------------------ Scc
; void fl_scc_table(UBYTE *taken, UWORD *sr)
; for ccr = 0..31: MOVE to CCR, then the 16 Scc in a row (Scc does not
; touch the CCR), each followed by MOVE from SR.
SCCSR	macro
	s\1	(a0)+
	move.w	sr,(a1)+
	endm
	xdef	_fl_scc_table
_fl_scc_table:
	SAVE
	movem.l	48(sp),a0-a1
	moveq	#0,d1
.l:	move.w	d1,ccr
	SCCSR	t
	SCCSR	f
	SCCSR	hi
	SCCSR	ls
	SCCSR	cc
	SCCSR	cs
	SCCSR	ne
	SCCSR	eq
	SCCSR	vc
	SCCSR	vs
	SCCSR	pl
	SCCSR	mi
	SCCSR	ge
	SCCSR	lt
	SCCSR	gt
	SCCSR	le
	addq.w	#1,d1
	cmp.w	#32,d1
	bne	.l
	REST
	rts

; ------------------------------------------------------------------ Bcc
; void fl_bcc_table(UBYTE *taken, UWORD *sr, ULONG wide)
; 16 entries per ccr: cond 0 = BRA, cond 1 has no Bcc (placeholder, $00),
; conds 2..15 = Bhi..Ble. All after one MOVE to CCR. wide=0: .s, else .w
BCCX	macro
	b\1.\2	.t\@
	sf	(a0)+
	bra.\2	.n\@
.t\@:	st	(a0)+
.n\@:	move.w	sr,(a1)+
	endm
BCCNONE	macro
	sf	(a0)+
	move.w	sr,(a1)+
	endm
BCC16	macro
	BCCX	ra,\1
	BCCNONE
	BCCX	hi,\1
	BCCX	ls,\1
	BCCX	cc,\1
	BCCX	cs,\1
	BCCX	ne,\1
	BCCX	eq,\1
	BCCX	vc,\1
	BCCX	vs,\1
	BCCX	pl,\1
	BCCX	mi,\1
	BCCX	ge,\1
	BCCX	lt,\1
	BCCX	gt,\1
	BCCX	le,\1
	endm
	xdef	_fl_bcc_table
_fl_bcc_table:
	SAVE
	movem.l	48(sp),a0-a1
	move.l	56(sp),d0
	moveq	#0,d1
	tst.l	d0
	bne	.w
.s:	move.w	d1,ccr
	BCC16	s
	addq.w	#1,d1
	cmp.w	#32,d1
	bne	.s
	REST
	rts
.w:	move.w	d1,ccr
	BCC16	w
	addq.w	#1,d1
	cmp.w	#32,d1
	bne	.w
	REST
	rts

; ------------------------------------------------------------------ DBcc
; void fl_dbcc_table(UBYTE *taken, UWORD *sr, ULONG *cnt)
; for start in ($12340001, $12340000), ccr 0..31, cond 0..15:
;   d2 = start; MOVE to CCR; DBcc d2,taken; record taken, SR, d2.
DBX	macro
	move.l	d7,d2
	move.w	d1,ccr
	db\1	d2,.t\@
	sf	(a0)+
	bra.s	.n\@
.t\@:	st	(a0)+
.n\@:	move.w	sr,(a1)+
	move.l	d2,(a2)+
	endm
	xdef	_fl_dbcc_table
_fl_dbcc_table:
	SAVE
	movem.l	48(sp),a0-a2
	move.l	#$12340001,d7
.o:	moveq	#0,d1
.l:	DBX	t
	DBX	f
	DBX	hi
	DBX	ls
	DBX	cc
	DBX	cs
	DBX	ne
	DBX	eq
	DBX	vc
	DBX	vs
	DBX	pl
	DBX	mi
	DBX	ge
	DBX	lt
	DBX	gt
	DBX	le
	addq.w	#1,d1
	cmp.w	#32,d1
	bne	.l
	cmp.l	#$12340000,d7
	beq.s	.x
	move.l	#$12340000,d7
	bra	.o
.x:	REST
	rts

; ------------------------------------------------------------------ xxxI to CCR
; void fl_ccrimm_table(UWORD *sr)
; for ccr 0..31: 36 entries (ORI, ANDI, EORI) x 12 immediates
CCROP	macro
	move.w	d1,ccr
	\1.b	#\2,ccr
	move.w	sr,(a0)+
	endm
CCR12	macro
	CCROP	\1,$00
	CCROP	\1,$01
	CCROP	\1,$02
	CCROP	\1,$04
	CCROP	\1,$08
	CCROP	\1,$10
	CCROP	\1,$1f
	CCROP	\1,$ff
	CCROP	\1,$e0
	CCROP	\1,$15
	CCROP	\1,$0a
	CCROP	\1,$1b
	endm
	xdef	_fl_ccrimm_table
_fl_ccrimm_table:
	SAVE
	move.l	48(sp),a0
	moveq	#0,d1
.l:	CCR12	ori
	CCR12	andi
	CCR12	eori
	addq.w	#1,d1
	cmp.w	#32,d1
	bne	.l
	REST
	rts

; ------------------------------------------------------------------ MOVE to CCR
; void fl_moveccr_table(UWORD *sr, const UWORD *memsrc)
; 256 register-form entries (source word = $A500 | v), then 32 memory-form
; entries from memsrc[], then 4 immediate-form entries.
	xdef	_fl_moveccr_table
_fl_moveccr_table:
	SAVE
	movem.l	48(sp),a0-a1
	moveq	#0,d1
.l:	move.w	d1,d2
	or.w	#$a500,d2
	move.w	d2,ccr
	move.w	sr,(a0)+
	addq.w	#1,d1
	cmp.w	#256,d1
	bne	.l
	moveq	#31,d1
.m:	move.w	(a1)+,ccr
	move.w	sr,(a0)+
	dbf	d1,.m
	move.w	#$ffe0,ccr
	move.w	sr,(a0)+
	move.w	#$0015,ccr
	move.w	sr,(a0)+
	move.w	#$ff1f,ccr
	move.w	sr,(a0)+
	move.w	#$800a,ccr
	move.w	sr,(a0)+
	REST
	rts

; ------------------------------------------------------------------ MOVE to/from SR
; void fl_movesr_table(UWORD *sr)
; for v 0..31: SR = $28E0|v (reserved bits 11, 7-5 set), read back
; with MOVE from SR (register and memory forms); SR back to $2000.
	xdef	_fl_movesr_table
_fl_movesr_table:
	SAVE
	move.l	48(sp),a0
	move.w	sr,d7			; restore value
	moveq	#0,d1
.l:	move.w	d1,d2
	or.w	#$28e0,d2
	move.w	d2,sr
	move.w	sr,d3
	move.w	sr,(a0)
	addq.l	#2,a0
	move.w	d3,(a0)+
	move.w	d7,sr
	addq.w	#1,d1
	cmp.w	#32,d1
	bne	.l
	REST
	rts

; ------------------------------------------------------------------ non-flag instructions
; void fl_preserve_table(UWORD *sr)
; for v 0..31: FL_NPRES entries, each MOVE to CCR / instruction / MOVE from SR.
; The order must match pres_names[] in flags.c.
PRE	macro
	move.w	d1,ccr
	endm
POST	macro
	move.w	sr,(a0)+
	endm
	xdef	_fl_preserve_table
_fl_preserve_table:
	SAVE
	move.l	48(sp),a0
	move.l	usp,a2			; USP, written back unchanged
	lea	fl_scratch,a3
	moveq	#0,d1
.l:	move.l	#$00018000,d4
	moveq	#0,d5
	lea	fl_scratch,a3
	PRE				; 0 movea.l
	movea.l	d4,a4
	POST
	PRE				; 1 movea.w
	movea.w	d4,a4
	POST
	PRE				; 2 lea
	lea	4(a3),a4
	POST
	PRE				; 3 pea
	pea	(a3)
	POST
	PRE				; 4 addq.l #4,sp
	addq.l	#4,sp
	POST
	PRE				; 5 adda.l
	adda.l	d4,a4
	POST
	PRE				; 6 adda.w
	adda.w	d4,a4
	POST
	PRE				; 7 suba.l
	suba.l	d4,a4
	POST
	PRE				; 8 addq.w #1,An
	addq.w	#1,a4
	POST
	PRE				; 9 subq.l #8,An
	subq.l	#8,a4
	POST
	PRE				; 10 exg dx,dy
	exg	d4,d5
	POST
	exg	d4,d5
	PRE				; 11 exg ax,ay
	exg	a3,a4
	POST
	exg	a3,a4
	PRE				; 12 exg dx,ay
	exg	d6,a4
	POST
	PRE				; 13 movem to -(sp)
	movem.l	d4-d5,-(sp)
	POST
	PRE				; 14 movem (sp)+
	movem.l	(sp)+,d4-d5
	POST
	PRE				; 15 link
	link	a6,#-8
	POST
	PRE				; 16 unlk
	unlk	a6
	POST
	PRE				; 17 move usp,an
	move.l	usp,a4
	POST
	PRE				; 18 move an,usp
	move.l	a2,usp
	POST
	PRE				; 19 nop
	nop
	POST
	PRE				; 20 bsr/rts
	bsr.w	fl_rts_sub
	POST
	PRE				; 21 jsr abs/rts
	jsr	fl_rts_sub
	POST
	PRE				; 22 bra.s
	bra.s	.b22
	nop
.b22:	POST
	lea	.j23,a4
	PRE				; 23 jmp (an)
	jmp	(a4)
	nop
.j23:	POST
	moveq	#0,d6
	PRE				; 24 dbf, counter expires
	dbf	d6,.d24
.d24:	POST
	moveq	#5,d6
	PRE				; 25 dbf, taken
	dbf	d6,.d25
	nop
.d25:	POST
	PRE				; 26 st dn
	st	d6
	POST
	PRE				; 27 move sr,dn
	move.w	sr,d6
	POST
	PRE				; 28 movem.w to memory
	movem.w	d4,(a3)
	POST
	PRE				; 29 movem.l from memory
	movem.l	(a3),d6
	POST
	PRE				; 30 lea (d8,an,xn)
	lea	2(a3,d5.w),a4
	POST
	PRE				; 31 swap ($00018000 -> $80000001: N=1)
	swap	d4
	POST
	PRE				; 32 swap (0 -> 0: Z=1)
	swap	d5
	POST
	addq.w	#1,d1
	cmp.w	#32,d1
	bne	.l
	REST
	rts
fl_rts_sub:
	rts

; ------------------------------------------------------------------ X vs C
; void fl_xc_table(UWORD *sr, UBYTE *x_roxl, UBYTE *x_addx, const UBYTE *presets, ULONG npre)
; For each preset ccr, FL_NXC instructions, three times each:
;   A: MOVE to CCR, insn, MOVE from SR            -> sr[]
;   B: d7=0, MOVE to CCR, insn, ROXL.L #1,d7      -> x_roxl[] (= X)
;   C: d7=0, MOVE to CCR, insn, ADDX.L d7,d7      -> x_addx[] (= X)
; Operands are reloaded before each MOVE to CCR. Order = xc_names[] in flags.c.
XCLOAD	macro
	moveq	#1,d0
	moveq	#2,d5
	move.l	#$80000001,d6
	move.l	#1,a3
	move.l	#2,a4
	lea	fl_xc_mem1,a5		; (a5) = 1
	lea	fl_xc_mem2,a6		; (a6) = 2
	moveq	#0,d7
	endm
XC	macro	; \1..: the instruction text is given by the XCI_\1 macro
	XCLOAD
	move.w	d1,ccr
	XCI_\1
	move.w	sr,(a0)+
	XCLOAD
	move.w	d1,ccr
	XCI_\1
	roxl.l	#1,d7
	move.b	d7,(a1)+
	XCLOAD
	move.w	d1,ccr
	XCI_\1
	addx.l	d7,d7
	move.b	d7,(a2)+
	endm
XCI_0	macro
	cmp.l	d5,d0
	endm
XCI_1	macro
	cmpi.l	#2,d0
	endm
XCI_2	macro
	cmpa.l	a4,a3
	endm
XCI_3	macro
	cmpm.l	(a6)+,(a5)+
	endm
XCI_4	macro
	tst.l	d0
	endm
XCI_5	macro
	and.l	d5,d0
	endm
XCI_6	macro
	or.l	d5,d0
	endm
XCI_7	macro
	eor.l	d5,d0
	endm
XCI_8	macro
	not.l	d0
	endm
XCI_9	macro
	clr.l	d0
	endm
XCI_10	macro
	move.l	d5,d0
	endm
XCI_11	macro
	moveq	#-1,d0
	endm
XCI_12	macro
	ext.l	d0
	endm
XCI_13	macro
	swap	d0
	endm
XCI_14	macro
	mulu.w	d5,d0
	endm
XCI_15	macro
	muls.w	d5,d0
	endm
XCI_16	macro
	divu.w	d5,d0
	endm
XCI_17	macro
	divs.w	d5,d0
	endm
XCI_18	macro
	btst	#0,d0
	endm
XCI_19	macro
	bset	#1,d0
	endm
XCI_20	macro
	bchg	#1,d0
	endm
XCI_21	macro
	bclr	#0,d0
	endm
XCI_22	macro
	rol.l	#1,d6
	endm
XCI_23	macro
	ror.l	#1,d6
	endm
XCI_24	macro
	rol.l	d5,d6
	endm
XCI_25	macro
	cmp.b	d5,d0
	endm
XCI_26	macro
	cmp.w	d5,d0
	endm
XCI_27	macro
	cmpa.w	a4,a3
	endm
XCI_28	macro
	addq.l	#1,a3
	endm
XCI_29	macro
	subq.w	#1,a3
	endm
XCI_30	macro
	ext.w	d0
	endm
XCI_31	macro
	move.w	d5,d0
	endm
	xdef	_fl_xc_table
_fl_xc_table:
	SAVE
	movem.l	48(sp),a0-a2
	move.l	60(sp),a3
	move.l	64(sp),d2
	subq.l	#1,d2
	move.l	a3,fl_xc_pre
.p:	move.l	fl_xc_pre,a3
	moveq	#0,d1
	move.b	(a3)+,d1
	move.l	a3,fl_xc_pre
	XC	0
	XC	1
	XC	2
	XC	3
	XC	4
	XC	5
	XC	6
	XC	7
	XC	8
	XC	9
	XC	10
	XC	11
	XC	12
	XC	13
	XC	14
	XC	15
	XC	16
	XC	17
	XC	18
	XC	19
	XC	20
	XC	21
	XC	22
	XC	23
	XC	24
	XC	25
	XC	26
	XC	27
	XC	28
	XC	29
	XC	30
	XC	31
	dbf	d2,.p
	REST
	rts

; ------------------------------------------------------------------ shifts/rotates
; ULONG fl_shift(ULONG val, ULONG cnt, ULONG ccr, ULONG which)
; returns the result register (or memory word), SR in fl_shift_sr.
; which: 0..23 = kind*3+size (kinds roxl roxr rol ror lsl lsr asl asr,
; sizes b w l) with register count; 24.. immediate and memory forms
; (see shift_imm[] in flags.c).
	xdef	_fl_shift
	xdef	_fl_shift_sr
_fl_shift:
	SAVE
	movem.l	48(sp),d0-d3
	lsl.w	#2,d3
	lea	.tab(pc),a0
	move.l	0(a0,d3.w),a0
	lea	fl_shift_mem,a1
	move.w	d0,(a1)
	move.w	d2,ccr
	jmp	(a0)
.tab:	dc.l	.x0,.x1,.x2,.x3,.x4,.x5,.x6,.x7,.x8,.x9,.x10,.x11
	dc.l	.x12,.x13,.x14,.x15,.x16,.x17,.x18,.x19,.x20,.x21,.x22,.x23
	dc.l	.i0,.i1,.i2,.i3,.i4,.i5,.i6,.i7,.m0,.m1
.x0:	roxl.b	d1,d0
	bra	.out
.x1:	roxl.w	d1,d0
	bra	.out
.x2:	roxl.l	d1,d0
	bra	.out
.x3:	roxr.b	d1,d0
	bra	.out
.x4:	roxr.w	d1,d0
	bra	.out
.x5:	roxr.l	d1,d0
	bra	.out
.x6:	rol.b	d1,d0
	bra	.out
.x7:	rol.w	d1,d0
	bra	.out
.x8:	rol.l	d1,d0
	bra	.out
.x9:	ror.b	d1,d0
	bra	.out
.x10:	ror.w	d1,d0
	bra	.out
.x11:	ror.l	d1,d0
	bra	.out
.x12:	lsl.b	d1,d0
	bra	.out
.x13:	lsl.w	d1,d0
	bra	.out
.x14:	lsl.l	d1,d0
	bra	.out
.x15:	lsr.b	d1,d0
	bra	.out
.x16:	lsr.w	d1,d0
	bra	.out
.x17:	lsr.l	d1,d0
	bra	.out
.x18:	asl.b	d1,d0
	bra	.out
.x19:	asl.w	d1,d0
	bra	.out
.x20:	asl.l	d1,d0
	bra	.out
.x21:	asr.b	d1,d0
	bra	.out
.x22:	asr.w	d1,d0
	bra	.out
.x23:	asr.l	d1,d0
	bra	.out
.i0:	roxl.l	#1,d0
	bra	.out
.i1:	roxl.l	#8,d0
	bra	.out
.i2:	roxr.w	#3,d0
	bra	.out
.i3:	roxr.b	#8,d0
	bra	.out
.i4:	roxl.b	#5,d0
	bra	.out
.i5:	asl.w	#8,d0
	bra	.out
.i6:	lsr.l	#4,d0
	bra	.out
.i7:	roxr.l	#1,d0
	bra	.out
.m0:	roxl.w	(a1)
	bra	.mout
.m1:	roxr.w	(a1)
.mout:	move.w	sr,_fl_shift_sr
	moveq	#0,d0
	move.w	(a1),d0
	REST
	rts
.out:	move.w	sr,_fl_shift_sr
	REST
	rts

; ------------------------------------------------------------------ multi-precision
; ULONG fl_x<op>_r4(ULONG *dst, const ULONG *src, ULONG ccr)
; dst[0..3] (dst[0] most significant) op= src[0..3], fully unrolled register
; form, least significant first. Returns the final SR.
XR4	macro	; \1 name \2 mnemonic
	xdef	_fl_\1_r4
_fl_\1_r4:
	SAVE
	movem.l	48(sp),a0-a1
	movem.l	(a0),d0-d3
	movem.l	(a1),d4-d7
	move.w	58(sp),ccr
	\2.l	d7,d3
	\2.l	d6,d2
	\2.l	d5,d1
	\2.l	d4,d0
	move.w	sr,-(sp)
	movem.l	d0-d3,(a0)
	moveq	#0,d0
	move.w	(sp)+,d0
	REST
	rts
	endm
	XR4	addx,addx
	XR4	subx,subx
	xdef	_fl_negx_r4
_fl_negx_r4:
	SAVE
	movem.l	48(sp),a0-a1
	movem.l	(a0),d0-d3
	move.w	58(sp),ccr
	negx.l	d3
	negx.l	d2
	negx.l	d1
	negx.l	d0
	move.w	sr,-(sp)
	movem.l	d0-d3,(a0)
	moveq	#0,d0
	move.w	(sp)+,d0
	REST
	rts

; ULONG fl_xmem_<op>(void *dst_end, const void *src_end, ULONG n, ULONG ccr)
; n elements, -(An),-(An) (or -(An) for NEGX / memory ROX) in a DBF loop,
; or (An)+ for the ROXR chains. ccr bit 16 set = also raise SOFTINT
; (INTREQ SETCLR|bit 2, written with MOVEM so no flag changes) just before
; the chain. Returns the final SR.
XMPRO	macro
	SAVE
	movem.l	48(sp),a0-a1
	movem.l	56(sp),d2-d3
	subq.l	#1,d2
	move.w	#$8004,d5
	lea	$dff09c,a5
	btst	#16,d3
	beq.s	.nt\@
	movem.w	d5,(a5)
.nt\@:	move.w	d3,ccr
	endm
XMEPI	macro
	move.w	sr,d0
	and.l	#$ffff,d0
	REST
	rts
	endm
XMEM	macro	; \1 mnemonic \2 size
	xdef	_fl_xmem_\1_\2
_fl_xmem_\1_\2:
	XMPRO
.l\@:	\1.\2	-(a1),-(a0)
	dbf	d2,.l\@
	XMEPI
	endm
	XMEM	addx,l
	XMEM	addx,w
	XMEM	addx,b
	XMEM	subx,l
	XMEM	subx,w
	XMEM	subx,b
	xdef	_fl_xmem_negx_l
_fl_xmem_negx_l:
	XMPRO
.l:	negx.l	-(a0)
	dbf	d2,.l
	XMEPI
	xdef	_fl_xmem_roxlw
_fl_xmem_roxlw:				; words, from the end, roxl.w -(a0)
	XMPRO
.l:	roxl.w	-(a0)
	dbf	d2,.l
	XMEPI
	xdef	_fl_xmem_roxrw
_fl_xmem_roxrw:				; words, from the start (a1 = start)
	XMPRO
.l:	roxr.w	(a1)+
	dbf	d2,.l
	XMEPI
	xdef	_fl_xmem_roxll
_fl_xmem_roxll:				; longs from the end via a register:
	XMPRO				; MOVE between keeps X, sets NZ, clears VC
.l:	move.l	-(a0),d0
	roxl.l	#1,d0
	move.l	d0,(a0)
	dbf	d2,.l
	XMEPI
	xdef	_fl_xmem_roxrl
_fl_xmem_roxrl:				; longs from the start (a1 = start)
	XMPRO
.l:	move.l	(a1),d0
	roxr.l	#1,d0
	move.l	d0,(a1)+
	dbf	d2,.l
	XMEPI

; ------------------------------------------------------------------ TRAP / RTE
; void fl_trap_table(UWORD *out)   (vectors 33-35 and 7 installed by C)
; for v 0..31 four words:
;   trap #1 (handler EORI #$1F,CCR; RTE)       -> SR after
;   trap #2 (handler EORI.W #$1F,(SP); RTE)    -> SR after
;   trap #3 (handler MOVE SR,D5; RTE)          -> D5 (SR seen in the handler)
;   TRAPV  (handler counts; RTE)               -> SR after
	xdef	_fl_trap_table
	xdef	_fl_trap1_h
	xdef	_fl_trap2_h
	xdef	_fl_trap3_h
	xdef	_fl_trapv_h
	xdef	_fl_trapv_count
_fl_trap_table:
	SAVE
	move.l	48(sp),a0
	moveq	#0,d1
.l:	move.w	d1,ccr
	trap	#1
	move.w	sr,(a0)+
	move.w	d1,ccr
	trap	#2
	move.w	sr,(a0)+
	moveq	#0,d5
	move.w	d1,ccr
	trap	#3
	move.w	d5,(a0)+
	move.w	d1,ccr
	trapv
	move.w	sr,(a0)+
	addq.w	#1,d1
	cmp.w	#32,d1
	bne	.l
	REST
	rts
_fl_trap1_h:
	eori.b	#$1f,ccr
	rte
_fl_trap2_h:
	eori.w	#$001f,(sp)
	rte
_fl_trap3_h:
	move.w	sr,d5
	rte
_fl_trapv_h:
	addq.l	#1,_fl_trapv_count
	rte

; ------------------------------------------------------------------ flags across gaps
; ULONG fg_<op>_<gap>(ULONG a, ULONG b, ULONG ccr, UBYTE *out)
; MOVE to CCR (preset), one flag-producing op on d0 (,d1), then a "gap"
; that must not change the flags, then the consumers:
;   MOVE from SR, the 16 Scc into out[0..15], X via ADDX into bits 8+,
;   the result d0 into out[16..19].
; Returns (SR & $1F) | X<<8 | (d6 & $FF)<<16.
OP_nop	macro
	endm
OP_addl	macro
	add.l	d1,d0
	endm
OP_addw	macro
	add.w	d1,d0
	endm
OP_addb	macro
	add.b	d1,d0
	endm
OP_subl	macro
	sub.l	d1,d0
	endm
OP_subw	macro
	sub.w	d1,d0
	endm
OP_subb	macro
	sub.b	d1,d0
	endm
OP_cmpl	macro
	cmp.l	d1,d0
	endm
OP_cmpb	macro
	cmp.b	d1,d0
	endm
OP_negl	macro
	neg.l	d0
	endm
OP_negw	macro
	neg.w	d0
	endm
OP_addxl	macro
	addx.l	d1,d0
	endm
OP_subxl	macro
	subx.l	d1,d0
	endm
OP_negxl	macro
	negx.l	d0
	endm
OP_andl	macro
	and.l	d1,d0
	endm
OP_orw	macro
	or.w	d1,d0
	endm
OP_lsll	macro
	lsl.l	#1,d0
	endm
OP_asrl	macro
	asr.l	#1,d0
	endm
OP_asll	macro
	asl.l	#1,d0
	endm
OP_roxll	macro
	roxl.l	#1,d0
	endm
OP_roxrl	macro
	roxr.l	#1,d0
	endm
OP_mulu	macro
	mulu.w	d1,d0
	endm
OP_tstl	macro
	tst.l	d0
	endm

GAP_none	macro
	endm
GAP_nonflag	macro
	movea.l	d0,a1
	lea	8(a1),a1
	adda.l	d1,a1
	suba.w	d1,a1
	addq.l	#4,a1
	subq.w	#2,a1
	exg	d4,d5
	exg	a1,a2
	exg	d5,a1
	movem.l	d4-d5,-(sp)
	movem.l	(sp)+,d4-d5
	pea	(a1)
	addq.l	#4,sp
	link	a6,#-4
	unlk	a6
	nop
	move.l	usp,a2
	st	d5
	sf	d5
	movem.w	d0,(a3)
	endm
GAP_branch	macro
	bhi.s	.h\@
	lea	1(a1),a1		; not taken path
	bra.s	.j\@
.h\@:	lea	2(a1),a1
.j\@:	bmi.w	.m\@
	lea	3(a1),a1
	bra.w	.k\@
.m\@:	lea	4(a1),a1
.k\@:	bvs.s	.v\@
	bcs.s	.v\@
	beq.s	.v\@
	bge.s	.v\@
	nop
.v\@:
	endm
GAP_bsr	macro
	bsr.w	fg_sub
	jsr	fg_sub2
	endm
GAP_dbcc	macro
.p\@:	addq.l	#1,a1
	dbf	d5,.p\@
.l\@:	lea	1(a1),a1
	dbeq	d6,.l\@
	endm
GAP_trap	macro
	trap	#1
	trap	#1
	endm
GAP_irq	macro
	movem.w	d3,(a5)
.w\@:	addq.l	#1,a1
	dbf	d7,.w\@
	endm

SCC16	macro
	st	(a4)+
	sf	(a4)+
	shi	(a4)+
	sls	(a4)+
	scc	(a4)+
	scs	(a4)+
	sne	(a4)+
	seq	(a4)+
	svc	(a4)+
	svs	(a4)+
	spl	(a4)+
	smi	(a4)+
	sge	(a4)+
	slt	(a4)+
	sgt	(a4)+
	sle	(a4)+
	endm

FG	macro	; \1 op \2 gap
fg_\1_\2:
	SAVE
	movem.l	48(sp),d0-d2/a4
	lea	fl_scratch,a3
	move.l	a3,a1
	move.l	a3,a2
	move.w	#7,d6
	move.w	#3,d5
	move.w	#47,d7
	move.w	#$8004,d3
	lea	$dff09c,a5
	move.w	d2,ccr
	OP_\1
	GAP_\2
	move.w	sr,d4
	SCC16
	moveq	#0,d7
	addx.l	d7,d7
	move.l	d0,(a4)+
	and.l	#$1f,d4
	lsl.w	#8,d7
	or.w	d7,d4
	and.l	#$ff,d6
	swap	d6
	or.l	d6,d4
	move.l	d4,d0
	REST
	rts
	endm

FGSET	macro	; \1 gap: same op order as fg_ops[] in flags.c
	FG	nop,\1
	FG	addl,\1
	FG	addw,\1
	FG	addb,\1
	FG	subl,\1
	FG	subw,\1
	FG	subb,\1
	FG	cmpl,\1
	FG	cmpb,\1
	FG	negl,\1
	FG	negw,\1
	FG	addxl,\1
	FG	subxl,\1
	FG	negxl,\1
	FG	andl,\1
	FG	orw,\1
	FG	lsll,\1
	FG	asrl,\1
	FG	asll,\1
	FG	roxll,\1
	FG	roxrl,\1
	FG	mulu,\1
	FG	tstl,\1
	endm
FGTAB	macro
	dc.l	fg_nop_\1,fg_addl_\1,fg_addw_\1,fg_addb_\1,fg_subl_\1,fg_subw_\1
	dc.l	fg_subb_\1,fg_cmpl_\1,fg_cmpb_\1,fg_negl_\1,fg_negw_\1,fg_addxl_\1
	dc.l	fg_subxl_\1,fg_negxl_\1,fg_andl_\1,fg_orw_\1,fg_lsll_\1,fg_asrl_\1
	dc.l	fg_asll_\1,fg_roxll_\1,fg_roxrl_\1,fg_mulu_\1,fg_tstl_\1
	endm

	FGSET	none
	FGSET	nonflag
	FGSET	branch
	FGSET	bsr
	FGSET	dbcc
	FGSET	trap
	FGSET	irq

fg_sub:					; non-flag subroutine body
	lea	4(a1),a1
	exg	a1,a2
	exg	a1,a2
	movea.l	a1,a0
	rts
fg_sub2:
	addq.l	#2,a1
	subq.l	#2,a1
	rts

	section	.data,data
	xdef	_fg_tab
_fg_tab:				; [gap][op], gap order = fg_gaps[] in flags.c
	FGTAB	none
	FGTAB	nonflag
	FGTAB	branch
	FGTAB	bsr
	FGTAB	dbcc
	FGTAB	trap
	FGTAB	irq
fl_xc_mem1:	dc.l	1,0
fl_xc_mem2:	dc.l	2,0

	section	.bss,bss
fl_scratch:	ds.l	16
fl_xc_pre:	ds.l	1
fl_shift_mem:	ds.l	1
_fl_shift_sr:	ds.w	2
_fl_trapv_count:	ds.l	1
