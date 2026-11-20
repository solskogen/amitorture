; bench_mmu helpers (mmu.c): MMU on/off, the timed loops and the bus error
; handler for the fault round trip. MMU instructions are dc.w so this
; assembles for -m68000; the C side only calls the family's routines.
; _bmm_fam: 3 = 68030, 4 = 68040/LC040, 6 = 68060/LC060.
	machine	68020
	opt	o-

	xdef	_bmm_fam,_bmm_tc,_bmm_tt,_bmm_crp,_bmm_root,_bmm_orig
	xdef	_bmm_fdesc,_bmm_finv,_bmm_fval,_bmm_fva,_bmm_fexp,_bmm_old2
	xdef	_bmm_save,_bmm_setup,_bmm_restore
	xdef	_bmm_read,_bmm_fault,_bmm_pflusha,_bmm_ptest,_bmm_berr
	xdef	_bmm_keep,_bmm_on_c,_bmm_off_c

	section	.data,data
_bmm_fam:	dc.w	0
_bmm_fexp:	dc.w	0
_bmm_keep:	dc.w	0		; set: the caller turns the MMU on/off around the whole block
_bmm_tc:	dc.l	0		; TC for the timed run (0 = MMU off)
_bmm_tt:	dc.l	0,0		; TT0/TT1 (DTT0/DTT1) for the timed run
_bmm_crp:	dc.l	0,0		; 030 root pointer of our tables
_bmm_root:	dc.l	0		; 040/060 root table
_bmm_fdesc:	dc.l	0		; fault page descriptor (physical)
_bmm_finv:	dc.l	0		; its invalid value
_bmm_fval:	dc.l	0		; its valid value
_bmm_fva:	dc.l	0		; fault page logical address
_bmm_old2:	dc.l	0		; previous bus error handler
; original registers: 0 tc, 4 crp hi, 8 crp lo, 12 srp hi, 16 srp lo,
; 20 tt0/dtt0, 24 tt1/dtt1, 28 urp, 32 srp40, 36 dfc, 40 cacr
_bmm_orig:	ds.l	11
bmm_zero:	dc.l	0

	section	.text,code

; void bmm_save(void): read the original MMU registers into bmm_orig
_bmm_save:
	lea	_bmm_orig,a0
	cmp.w	#3,_bmm_fam
	bne.s	.s4
	dc.w	$f010,$4200		; pmove tc,(a0)
	addq.l	#4,a0
	dc.w	$f010,$4e00		; pmove crp,(a0)
	addq.l	#8,a0
	dc.w	$f010,$4a00		; pmove srp,(a0)
	addq.l	#8,a0
	dc.w	$f010,$0a00		; pmove tt0,(a0)
	addq.l	#4,a0
	dc.w	$f010,$0e00		; pmove tt1,(a0)
	lea	_bmm_orig,a0
	dc.w	$4e7a,$0002		; movec cacr,d0
	move.l	d0,40(a0)
	rts
.s4:	dc.w	$4e7a,$0003		; movec tc,d0
	move.l	d0,(a0)
	dc.w	$4e7a,$0006		; movec dtt0,d0
	move.l	d0,20(a0)
	dc.w	$4e7a,$0007		; movec dtt1,d0
	move.l	d0,24(a0)
	dc.w	$4e7a,$0806		; movec urp,d0
	move.l	d0,28(a0)
	dc.w	$4e7a,$0807		; movec srp,d0
	move.l	d0,32(a0)
	dc.w	$4e7a,$0001		; movec dfc,d0
	move.l	d0,36(a0)
	dc.w	$4e7a,$0002		; movec cacr,d0
	move.l	d0,40(a0)
	rts

; void bmm_setup(void): load our root pointers (TC stays off) and DFC = 5
_bmm_setup:
	cmp.w	#3,_bmm_fam
	bne.s	.u4
	lea	_bmm_crp,a0
	dc.w	$f010,$4c00		; pmove (a0),crp
	dc.w	$f000,$2400		; pflusha
	rts
.u4:	move.l	_bmm_root,d0
	nop
	dc.w	$4e7b,$0806		; movec d0,urp
	dc.w	$4e7b,$0807		; movec d0,srp
	moveq	#5,d0
	dc.w	$4e7b,$0001		; movec d0,dfc
	dc.w	$f518			; pflusha
	nop
	rts

; void bmm_restore(void): put the original registers back
_bmm_restore:
	lea	_bmm_orig,a0
	cmp.w	#3,_bmm_fam
	bne.s	.r4
	lea	bmm_zero,a1
	dc.w	$f011,$4000		; pmove (a1),tc
	move.l	4(a0),d0
	and.w	#3,d0
	beq.s	.r3			; DT=0 would take a configuration exception
	addq.l	#4,a0
	dc.w	$f010,$4c00		; pmove (a0),crp
	lea	_bmm_orig,a0
.r3:	dc.w	$f010,$4000		; pmove (a0),tc
	dc.w	$f000,$2400		; pflusha
	rts
.r4:	nop
	moveq	#0,d0
	dc.w	$4e7b,$0003		; movec d0,tc
	nop
	move.l	28(a0),d0
	dc.w	$4e7b,$0806		; movec d0,urp
	move.l	32(a0),d0
	dc.w	$4e7b,$0807		; movec d0,srp
	move.l	36(a0),d0
	dc.w	$4e7b,$0001		; movec d0,dfc
	move.l	(a0),d0
	dc.w	$4e7b,$0003		; movec d0,tc
	nop
	dc.w	$f518			; pflusha
	nop
	rts

; the timed routines' on/off: nothing when mmu.c keeps the MMU on for the
; whole block (bmm_keep), so the timed region holds only the work
bmm_onk:
	tst.w	_bmm_keep
	beq.s	bmm_on
	rts
bmm_offk:
	tst.w	_bmm_keep
	beq.s	bmm_off
	rts

; MMU on with _bmm_tc (0 for the "off" block) and off again. Both flush the
; ATC and the caches so every block starts the same way. d0/a0 clobbered.
; Callable from C (void bmm_on_c(void), void bmm_off_c(void)).
_bmm_on_c:
bmm_on:
	cmp.w	#3,_bmm_fam
	bne.s	.n4
	dc.w	$f000,$2400		; pflusha
	dc.w	$4e7a,$0002		; movec cacr,d0
	or.w	#$0808,d0		; clear both caches
	dc.w	$4e7b,$0002		; movec d0,cacr
	lea	_bmm_tt,a0
	dc.w	$f010,$0800		; pmove (a0),tt0
	addq.l	#4,a0
	dc.w	$f010,$0c00		; pmove (a0),tt1
	lea	_bmm_tc,a0
	dc.w	$f010,$4000		; pmove (a0),tc
	rts
.n4:	nop
	dc.w	$f4f8			; cpusha bc
	cmp.w	#6,_bmm_fam
	bne.s	.n5
	dc.w	$f4d8			; 060: cinva bc
.n5:	dc.w	$f518			; pflusha
	move.l	_bmm_tt,d0
	dc.w	$4e7b,$0006		; movec d0,dtt0
	move.l	_bmm_tt+4,d0
	dc.w	$4e7b,$0007		; movec d0,dtt1
	move.l	_bmm_tc,d0
	dc.w	$4e7b,$0003		; movec d0,tc
	nop
	rts

_bmm_off_c:
bmm_off:
	cmp.w	#3,_bmm_fam
	bne.s	.f4
	lea	bmm_zero,a0
	dc.w	$f010,$4000		; pmove (a0),tc
	lea	_bmm_orig+20,a0
	dc.w	$f010,$0800		; pmove (a0),tt0
	addq.l	#4,a0
	dc.w	$f010,$0c00		; pmove (a0),tt1
	dc.w	$f000,$2400		; pflusha
	dc.w	$4e7a,$0002		; movec cacr,d0
	or.w	#$0808,d0
	dc.w	$4e7b,$0002		; movec d0,cacr
	rts
.f4:	nop
	moveq	#0,d0
	dc.w	$4e7b,$0003		; movec d0,tc
	nop
	move.l	_bmm_orig+20,d0
	dc.w	$4e7b,$0006		; movec d0,dtt0
	move.l	_bmm_orig+24,d0
	dc.w	$4e7b,$0007		; movec d0,dtt1
	dc.w	$f518			; pflusha
	dc.w	$f4f8			; cpusha bc
	cmp.w	#6,_bmm_fam
	bne.s	.f5
	dc.w	$f4d8			; cinva bc
.f5:	nop
	rts

BM_IN	macro
	movem.l	d2-d7/a2-a6,-(sp)
	endm
BM_OUT	macro
	movem.l	(sp)+,d2-d7/a2-a6
	rts
	endm

; ULONG bmm_read(ULONG count, ULONG base, ULONG step): per count 64 groups
; of four longword reads 4 KB + 16 bytes apart, then base += step. The
; sum is the same for every block (they all read the same physical data).
_bmm_read:
	BM_IN
	bsr	bmm_onk
	move.l	48(sp),d6
	move.l	52(sp),a4
	move.l	56(sp),d5
	moveq	#0,d0
.o:	move.l	a4,a0
	moveq	#63,d7
.i:	add.l	(a0),d0
	add.l	$1010(a0),d0
	add.l	$2020(a0),d0
	add.l	$3030(a0),d0
	add.l	d5,a0
	dbf	d7,.i
	subq.l	#1,d6
	bne.s	.o
	move.l	d0,d2
	bsr	bmm_offk
	move.l	d2,d0
	BM_OUT

; ULONG bmm_fault(ULONG count): per count make the fault page invalid,
; flush its ATC entry and read it: the bus error handler validates the
; descriptor and returns, and the read completes.
_bmm_fault:
	BM_IN
	bsr	bmm_onk
	move.l	48(sp),d6
	move.l	_bmm_fdesc,a2
	move.l	_bmm_fva,a3
	move.l	_bmm_finv,d4
	moveq	#0,d0
	move.w	#1,_bmm_fexp
	cmp.w	#3,_bmm_fam
	bne.s	.l4
.l3:	move.l	d4,(a2)
	dc.w	$f013,$3810		; pflush #0,#0,(a3)
	nop
	move.l	(a3),d1
	add.l	d1,d0
	subq.l	#1,d6
	bne.s	.l3
	bra.s	.x
.l4:	move.l	d4,(a2)
	nop
	dc.w	$f46a			; cpushl dc,(a2)
	dc.w	$f50b			; pflush (a3)
	nop
	move.l	(a3),d1
	add.l	d1,d0
	subq.l	#1,d6
	bne.s	.l4
.x:	clr.w	_bmm_fexp
	move.l	d0,d2
	bsr	bmm_offk
	move.l	d2,d0
	BM_OUT

; bus error (vector 2) while bench_mmu is installed
_bmm_berr:
	tst.w	_bmm_fexp
	beq.s	.chain
	move.l	a0,-(sp)
	move.l	_bmm_fdesc,a0
	move.l	_bmm_fval,(a0)
	cmp.w	#3,_bmm_fam
	bne.s	.h4
	move.l	_bmm_fva,a0
	dc.w	$f010,$3810		; pflush #0,#0,(a0)
	move.l	(sp)+,a0
	rte
.h4:	nop
	dc.w	$f468			; cpushl dc,(a0)
	move.l	_bmm_fva,a0
	dc.w	$f508			; pflush (a0)
	nop
	move.l	(sp)+,a0
	rte
.chain:	movem.l	d0/a0,-(sp)		; not ours: MMU off, then the old handler
	bsr	bmm_off
	movem.l	(sp)+,d0/a0
	move.l	_bmm_old2,-(sp)
	rts

; ULONG bmm_pflusha(ULONG count): 16 PFLUSHA per count, MMU on
_bmm_pflusha:
	BM_IN
	bsr	bmm_onk
	move.l	48(sp),d6
	cmp.w	#3,_bmm_fam
	bne.s	.p4
.p3:	rept	16
	dc.w	$f000,$2400		; pflusha
	endr
	subq.l	#1,d6
	bne.s	.p3
	bra.s	.px
.p4:	rept	16
	dc.w	$f518			; pflusha
	endr
	subq.l	#1,d6
	bne.s	.p4
.px:	bsr	bmm_offk
	moveq	#0,d0
	BM_OUT

; ULONG bmm_ptest(ULONG count, ULONG la): 16 address lookups per count, MMU
; on: 030 PTESTR #5,(a3),#7 (a full walk), 040 PTESTR (a3), 060 PLPAR (a3)
_bmm_ptest:
	BM_IN
	bsr	bmm_onk
	move.l	48(sp),d6
	move.l	52(sp),a4
	cmp.w	#4,_bmm_fam
	beq.w	.t4
	bhi.w	.t6
.t3:	rept	16
	move.l	a4,a3
	dc.w	$f013,$9e15		; ptestr #5,(a3),#7
	endr
	subq.l	#1,d6
	bne.s	.t3
	bra.w	.tx
.t4:	rept	16
	move.l	a4,a3
	dc.w	$f56b			; ptestr (a3)
	endr
	subq.l	#1,d6
	bne.s	.t4
	bra.w	.tx
.t6:	rept	16
	move.l	a4,a3
	dc.w	$f5cb			; plpar (a3)
	endr
	subq.l	#1,d6
	bne.s	.t6
.tx:	move.l	a3,d2
	bsr	bmm_offk
	move.l	d2,d0
	BM_OUT
