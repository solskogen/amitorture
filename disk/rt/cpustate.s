; amitorture: save/restore CPU control state around each bare module, so an
; aborted module cannot leave caches, MMU, 060 PCR or FPU control registers
; changed for the next module or the OS (CONTRACT.md section 10).
; Supervisor mode only. Every privileged/coprocessor instruction is dc.w
; encoded so this object assembles for the 68000; the C caller (mod.c) only
; calls what the probe says exists.
;
; struct amt_cpustate (offsets):
;   0 cacr   4 pcr   8 tc   12 tt0/itt0   16 tt1/itt1   20 dtt0   24 dtt1
;  28 urp   32 srp  36 crp030(8)  44 srp030(8)  52 fpcr  56 fpsr  60 fpiar

	xdef	_amt_cs_cacr_get
	xdef	_amt_cs_cacr_set
	xdef	_amt_cs_pcr_get
	xdef	_amt_cs_pcr_set
	xdef	_amt_cs_mmu040_save
	xdef	_amt_cs_mmu040_restore
	xdef	_amt_cs_mmu030_save
	xdef	_amt_cs_mmu030_restore
	xdef	_amt_cs_fpu_save
	xdef	_amt_cs_fpu_restore
	xdef	_amt_cs_cpusha

	section	.text,code

; ULONG amt_cs_cacr_get(void)                       (68020+)
_amt_cs_cacr_get:
	dc.w	$4e7a,$0002		; movec cacr,d0
	rts
; void amt_cs_cacr_set(ULONG)                       (68020+)
_amt_cs_cacr_set:
	move.l	4(sp),d0
	dc.w	$4e7b,$0002		; movec d0,cacr
	rts
; void amt_cs_cpusha(void)  push+invalidate both caches   (68040/68060)
_amt_cs_cpusha:
	dc.w	$f4f8			; cpusha bc
	rts
; ULONG amt_cs_pcr_get(void) / void amt_cs_pcr_set(ULONG)   (68060)
_amt_cs_pcr_get:
	dc.w	$4e7a,$0808		; movec pcr,d0
	rts
_amt_cs_pcr_set:
	move.l	4(sp),d0
	dc.w	$4e7b,$0808		; movec d0,pcr
	rts

; void amt_cs_mmu040_save(struct amt_cpustate *)     (68040/68060)
; has_mmu in the second argument: 0 = EC (only ITT/DTT exist)
_amt_cs_mmu040_save:
	move.l	4(sp),a0
	dc.w	$4e7a,$0004		; movec itt0,d0
	move.l	d0,12(a0)
	dc.w	$4e7a,$0005		; movec itt1,d0
	move.l	d0,16(a0)
	dc.w	$4e7a,$0006		; movec dtt0,d0
	move.l	d0,20(a0)
	dc.w	$4e7a,$0007		; movec dtt1,d0
	move.l	d0,24(a0)
	tst.l	8(sp)
	beq.s	.n
	dc.w	$4e7a,$0003		; movec tc,d0
	move.l	d0,8(a0)
	dc.w	$4e7a,$0806		; movec urp,d0
	move.l	d0,28(a0)
	dc.w	$4e7a,$0807		; movec srp,d0
	move.l	d0,32(a0)
.n:	rts

; void amt_cs_mmu040_restore(const struct amt_cpustate *, int has_mmu)
_amt_cs_mmu040_restore:
	move.l	4(sp),a0
	tst.l	8(sp)
	beq.s	.tt
	moveq	#0,d0
	dc.w	$4e7b,$0003		; movec d0,tc       translation off first
	move.l	28(a0),d0
	dc.w	$4e7b,$0806		; movec d0,urp
	move.l	32(a0),d0
	dc.w	$4e7b,$0807		; movec d0,srp
.tt:	move.l	12(a0),d0
	dc.w	$4e7b,$0004		; movec d0,itt0
	move.l	16(a0),d0
	dc.w	$4e7b,$0005		; movec d0,itt1
	move.l	20(a0),d0
	dc.w	$4e7b,$0006		; movec d0,dtt0
	move.l	24(a0),d0
	dc.w	$4e7b,$0007		; movec d0,dtt1
	tst.l	8(sp)
	beq.s	.n
	dc.w	$f518			; pflusha
	move.l	8(a0),d0
	dc.w	$4e7b,$0003		; movec d0,tc
.n:	rts

; void amt_cs_mmu030_save(struct amt_cpustate *, int has_mmu)   (68030/EC030)
_amt_cs_mmu030_save:
	move.l	4(sp),a0
	dc.w	$f028,$0a00,$000c	; pmove tt0,12(a0)
	dc.w	$f028,$0e00,$0010	; pmove tt1,16(a0)
	tst.l	8(sp)
	beq.s	.n
	dc.w	$f028,$4200,$0008	; pmove tc,8(a0)
	dc.w	$f028,$4e00,$0024	; pmove crp,36(a0)
	dc.w	$f028,$4a00,$002c	; pmove srp,44(a0)
.n:	rts

; void amt_cs_mmu030_restore(const struct amt_cpustate *, int has_mmu)
; CRP/SRP are reloaded only if their saved DT is valid (DT=0 would raise a
; configuration exception); TC last, after the root pointers.
_amt_cs_mmu030_restore:
	move.l	4(sp),a0
	tst.l	8(sp)
	beq.s	.tt
	clr.l	-(sp)
	dc.w	$f017,$4000		; pmove (sp),tc     translation off first
	addq.l	#4,sp
	move.l	36(a0),d0
	and.w	#3,d0
	beq.s	.nocrp
	dc.w	$f028,$4c00,$0024	; pmove 36(a0),crp
.nocrp:	move.l	44(a0),d0
	and.w	#3,d0
	beq.s	.tt
	dc.w	$f028,$4800,$002c	; pmove 44(a0),srp
.tt:	dc.w	$f028,$0800,$000c	; pmove 12(a0),tt0
	dc.w	$f028,$0c00,$0010	; pmove 16(a0),tt1
	tst.l	8(sp)
	beq.s	.n
	dc.w	$f000,$2400		; pflusha
	dc.w	$f028,$4000,$0008	; pmove 8(a0),tc
.n:	rts

; void amt_cs_fpu_save(struct amt_cpustate *) / restore    (FPU present, enabled)
_amt_cs_fpu_save:
	move.l	4(sp),a0
	dc.w	$f200,$b000		; fmove.l fpcr,d0
	move.l	d0,52(a0)
	dc.w	$f200,$a800		; fmove.l fpsr,d0
	move.l	d0,56(a0)
	dc.w	$f200,$a400		; fmove.l fpiar,d0
	move.l	d0,60(a0)
	rts
_amt_cs_fpu_restore:
	move.l	4(sp),a0
	move.l	52(a0),d0
	dc.w	$f200,$9000		; fmove.l d0,fpcr
	move.l	56(a0),d0
	dc.w	$f200,$8800		; fmove.l d0,fpsr
	move.l	60(a0),d0
	dc.w	$f200,$8400		; fmove.l d0,fpiar
	rts
