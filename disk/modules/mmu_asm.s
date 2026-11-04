;
; mmu module (id 20): run harness, recording exception stubs and the MMU
; instruction helpers.
;
; Assembled with -m68000 (no CPU suffix): every MMU/cache/MOVEC/MOVES
; instruction is written as dc.w so the exact encoding is visible here, and
; C only calls the 030 helpers on a 68030 and the 040/060 helpers on a
; 68040/68060.
;
; mm_run(fn)
;   SR=$2700, loads the MMU configuration in mm_cfg (unless mm_cfg.flags bit 0:
;   then the MMU is left off), sets d0-d7/a0-a6 from mm_in[15] and jumps to fn.
;   fn returns with RTS (registers are stored in mm_out[15]) or ends with an
;   exception. Afterwards the MMU is off again (TC = 0, the original TT
;   registers back, ATC flushed, caches pushed/cleared) and the SR and the
;   callee-saved registers of the caller are restored. Returns the vector
;   that ended fn (0 = returned normally, $ffff = runaway).
;
; Exception stubs (vectors 2-23, 32-63, installed by C with amt_vec_set)
;   mm_active = 0: chain to the previous handler (mm_old[vec]).
;   mm_active = 1: append {vec, handler SR, SSP, 46 frame words} to mm_log,
;                  then call mm_hook(index) if set: a non-zero result returns
;                  with RTE (the hook may have edited the frame or the
;                  tables); zero or no hook ends fn: the registers at the
;                  exception go to mm_out and mm_run continues with the
;                  MMU off. Nested exceptions (inside a hook) work the same.
;
	xdef	_mm_run,_mm_stub_table,_mm_old,_mm_active,_mm_hook
	xdef	_mm_cfg,_mm_orig,_mm_fam,_mm_in,_mm_out,_mm_vec,_mm_nlog,_mm_log
	xdef	_mm_usp,_mm_now
	xdef	_mm_save_orig,_mm_restore_orig,_mm_read_now
	xdef	_mm_pflusha,_mm_nop
	xdef	_p30_ptest,_p30_an,_p30_pload,_p30_pflush,_p30_dclr
	xdef	_p40_ptest,_p40_pflush,_p60_plpa,_p40_cpusha_dc,_p40_cinvl_dc
	xdef	_p40_cpushl_dc
	xdef	_mm_moves_get,_mm_moves_put
	xdef	_sn_rd,_sn_rd_at,_sn_wr,_sn_wr_at,_sn_addq,_sn_addq_at
	xdef	_sn_rdpi,_sn_rdpi_at,_sn_wrpi,_sn_wrpi_at
	xdef	_sn_urd,_sn_urd_at,_sn_uwr,_sn_uwr_at
	xdef	_sn_moves_wr,_sn_moves_wr_at
	xdef	_sn_plpar,_sn_plpar_at,_sn_plpaw,_sn_plpaw_at
	xdef	_sn_cfg_rp,_sn_cfg_rp_at,_sn_cfg_tc,_sn_cfg_tc_at
	xdef	_sn_load_rp30,_sn_tas,_sn_tas_at

MLOGMAX	equ	6
LOGSZ	equ	100			; vec.w, hsr.w, ssp.l, 46 frame words

; mm_regs layout (shared by mm_cfg and mm_orig, see mmu.c)
R_TC	equ	0
R_CRP	equ	4
R_SRP	equ	12
R_TT0	equ	20			; 030 TT0 / 040 DTT0
R_TT1	equ	24			; 030 TT1 / 040 DTT1
R_URP	equ	28
R_ITT0	equ	32
R_ITT1	equ	36
R_CACR	equ	40			; cfg: bits ORed into CACR; orig: CACR

; CPUSHA BC, and on the 68060 CINVA BC after it: with CACR.DPI (bit 28) set
; the 060's CPUSH pushes without invalidating (MC68060UM 5.3 CACR), and the
; C code then reads descriptors/data that must come from memory
PUSHBC	macro
	dc.w	$f4f8			; cpusha bc
	cmp.w	#6,_mm_fam
	bne.s	.pb\@
	dc.w	$f4d8			; cinva bc
.pb\@:
	endm
R_FLAGS	equ	44			; cfg bit 0: leave the MMU off
R_MMUSR	equ	48

	section	.text,code

; ------------------------------------------------------------------ run
_mm_run:
	movem.l	d2-d7/a2-a6,-(sp)
	move.w	sr,-(sp)
	move.l	sp,mm_save_sp
	move.l	2+44+4(sp),a0		; fn
	move.l	a0,mm_fn
	move.w	#$2700,sr
	move.l	usp,a1
	move.l	a1,mm_usp_save		; user-mode snippets change USP
	dc.w	$4e7a,$0000		; movec sfc,d0 (MOVES/PLPA snippets set
	move.l	d0,mm_sfc_save		; SFC/DFC: put back in mm_cont)
	dc.w	$4e7a,$0001		; movec dfc,d0
	move.l	d0,mm_dfc_save
	clr.w	_mm_vec
	clr.w	_mm_nlog
	btst	#0,_mm_cfg+R_FLAGS+3
	bne.s	.noen
	bsr	mm_enable
.noen:	move.w	#1,_mm_active
	pea	mm_ret(pc)
	move.l	mm_fn,-(sp)
	movem.l	_mm_in,d0-d7/a0-a6
	rts				; -> fn, which returns to mm_ret
mm_ret:
	movem.l	d0-d7/a0-a6,_mm_out
mm_cont:
	move.w	#$2700,sr
	clr.w	_mm_active
	move.l	mm_save_sp,sp
	bsr	mm_disable
	move.l	mm_usp_save,a1
	move.l	a1,usp
	move.l	mm_sfc_save,d0
	dc.w	$4e7b,$0000		; movec d0,sfc
	move.l	mm_dfc_save,d0
	dc.w	$4e7b,$0001		; movec d0,dfc
	move.w	(sp)+,sr
	movem.l	(sp)+,d2-d7/a2-a6
	moveq	#0,d0
	move.w	_mm_vec,d0
	rts

; enable: MMU off, load roots/TT, flush ATC, push/clear caches, set CACR
; bits, then TC
mm_enable:
	cmp.w	#3,_mm_fam
	bne	.e40
	lea	mm_zero,a0
	dc.w	$f010,$4000		; pmove (a0),tc
	lea	_mm_cfg+R_TT0,a0
	dc.w	$f010,$0800		; pmove (a0),tt0
	lea	_mm_cfg+R_TT1,a0
	dc.w	$f010,$0c00		; pmove (a0),tt1
	lea	_mm_cfg+R_CRP,a0
	dc.w	$f010,$4c00		; pmove (a0),crp
	lea	_mm_cfg+R_SRP,a0
	dc.w	$f010,$4800		; pmove (a0),srp
	dc.w	$f000,$2400		; pflusha
	dc.w	$4e7a,$0002		; movec cacr,d0
	move.l	d0,mm_cacr_save
	or.l	_mm_cfg+R_CACR,d0
	or.w	#$0808,d0		; CD|CI: clear both caches
	dc.w	$4e7b,$0002		; movec d0,cacr
	lea	_mm_cfg+R_TC,a0
	dc.w	$f010,$4000		; pmove (a0),tc
	rts
.e40:	moveq	#0,d0
	dc.w	$4e7b,$0003		; movec d0,tc
	move.l	_mm_cfg+R_URP,d0
	dc.w	$4e7b,$0806		; movec d0,urp
	move.l	_mm_cfg+R_SRP,d0
	dc.w	$4e7b,$0807		; movec d0,srp
	move.l	_mm_cfg+R_ITT0,d0
	dc.w	$4e7b,$0004		; movec d0,itt0
	move.l	_mm_cfg+R_ITT1,d0
	dc.w	$4e7b,$0005		; movec d0,itt1
	move.l	_mm_cfg+R_TT0,d0
	dc.w	$4e7b,$0006		; movec d0,dtt0
	move.l	_mm_cfg+R_TT1,d0
	dc.w	$4e7b,$0007		; movec d0,dtt1
	nop
	dc.w	$f518			; pflusha
	PUSHBC
	dc.w	$4e7a,$0002		; movec cacr,d0
	move.l	d0,mm_cacr_save
	or.l	_mm_cfg+R_CACR,d0
	dc.w	$4e7b,$0002		; movec d0,cacr
	nop
	move.l	_mm_cfg+R_TC,d0
	dc.w	$4e7b,$0003		; movec d0,tc
	nop
	rts

; disable: TC = 0, original TT registers, flush ATC, caches pushed/cleared,
; CACR as before mm_enable (root pointers stay; restored at the end)
mm_disable:
	btst	#0,_mm_cfg+R_FLAGS+3
	bne	.d30tt
	cmp.w	#3,_mm_fam
	bne	.d40
	lea	mm_zero,a0
	dc.w	$f010,$4000		; pmove (a0),tc
	dc.w	$f000,$2400		; pflusha
	move.l	mm_cacr_save,d0
	or.w	#$0808,d0
	dc.w	$4e7b,$0002		; movec d0,cacr (clears both caches)
.d30tt:	cmp.w	#3,_mm_fam
	bne	.d40tt
	lea	mm_zero,a0
	dc.w	$f010,$4000		; pmove (a0),tc  (TC off before the TTs)
	lea	_mm_orig+R_TT0,a0
	dc.w	$f010,$0800		; pmove (a0),tt0
	lea	_mm_orig+R_TT1,a0
	dc.w	$f010,$0c00		; pmove (a0),tt1
	lea	mm_zero,a0
	dc.w	$f010,$4000		; pmove (a0),tc
	dc.w	$f000,$2400		; pflusha
	rts
.d40:	nop
	moveq	#0,d0
	dc.w	$4e7b,$0003		; movec d0,tc
	nop
	dc.w	$f518			; pflusha
	PUSHBC
	move.l	mm_cacr_save,d0
	dc.w	$4e7b,$0002		; movec d0,cacr
	nop
.d40tt:	move.l	_mm_orig+R_ITT0,d0
	dc.w	$4e7b,$0004
	move.l	_mm_orig+R_ITT1,d0
	dc.w	$4e7b,$0005
	move.l	_mm_orig+R_TT0,d0
	dc.w	$4e7b,$0006
	move.l	_mm_orig+R_TT1,d0
	dc.w	$4e7b,$0007
	moveq	#0,d0
	dc.w	$4e7b,$0003		; movec d0,tc
	dc.w	$f518			; pflusha
	rts

; ------------------------------------------------------------------ regs
; mm_save_orig(): read every MMU register into mm_orig
_mm_save_orig:
	lea	_mm_orig,a1
	bra.s	mm_read
; mm_read_now(): read them into mm_now
_mm_read_now:
	lea	_mm_now,a1
mm_read:
	dc.w	$4e7a,$0002		; movec cacr,d0
	move.l	d0,R_CACR(a1)
	cmp.w	#3,_mm_fam
	bne.s	.r40
	lea	R_TC(a1),a0
	dc.w	$f010,$4200		; pmove tc,(a0)
	lea	R_CRP(a1),a0
	dc.w	$f010,$4e00		; pmove crp,(a0)
	lea	R_SRP(a1),a0
	dc.w	$f010,$4a00		; pmove srp,(a0)
	lea	R_TT0(a1),a0
	dc.w	$f010,$0a00		; pmove tt0,(a0)
	lea	R_TT1(a1),a0
	dc.w	$f010,$0e00		; pmove tt1,(a0)
	clr.w	R_MMUSR(a1)
	lea	R_MMUSR+2(a1),a0
	dc.w	$f010,$6200		; pmove mmusr,(a0)
	rts
.r40:	dc.w	$4e7a,$0003		; movec tc,d0
	move.l	d0,R_TC(a1)
	dc.w	$4e7a,$0806		; movec urp,d0
	move.l	d0,R_URP(a1)
	dc.w	$4e7a,$0807		; movec srp,d0
	move.l	d0,R_SRP(a1)
	dc.w	$4e7a,$0004
	move.l	d0,R_ITT0(a1)
	dc.w	$4e7a,$0005
	move.l	d0,R_ITT1(a1)
	dc.w	$4e7a,$0006
	move.l	d0,R_TT0(a1)
	dc.w	$4e7a,$0007
	move.l	d0,R_TT1(a1)
	clr.l	R_MMUSR(a1)
	cmp.w	#4,_mm_fam
	bne.s	.r60
	dc.w	$4e7a,$0805		; movec mmusr,d0 (040 only)
	move.l	d0,R_MMUSR(a1)
.r60:	rts

; mm_restore_orig(): put back TC/TT (030) or TC/URP/SRP/ITT/DTT (040/060).
; The 030 root pointers are restored by C through sn_load_rp30 (loading a
; root pointer with DT=0 takes an MMU configuration exception).
; Also used after an abort with the MMU on (mm_disable skipped): the caches
; are pushed and CACR is put back too.
_mm_restore_orig:
	move.w	sr,-(sp)
	move.w	#$2700,sr
	cmp.w	#3,_mm_fam
	bne.s	.o40
	lea	mm_zero,a0
	dc.w	$f010,$4000		; pmove (a0),tc  (off first)
	lea	_mm_orig+R_TT0,a0
	dc.w	$f010,$0800
	lea	_mm_orig+R_TT1,a0
	dc.w	$f010,$0c00
	dc.w	$f000,$2400		; pflusha
	lea	_mm_orig+R_TC,a0
	dc.w	$f010,$4000		; pmove (a0),tc
	move.l	_mm_orig+R_CACR,d0
	or.w	#$0808,d0		; CD|CI: clear both (write-through) caches
	dc.w	$4e7b,$0002		; movec d0,cacr
	move.w	(sp)+,sr
	rts
.o40:	nop
	PUSHBC
	moveq	#0,d0
	dc.w	$4e7b,$0003
	move.l	_mm_orig+R_URP,d0
	dc.w	$4e7b,$0806
	move.l	_mm_orig+R_SRP,d0
	dc.w	$4e7b,$0807
	move.l	_mm_orig+R_ITT0,d0
	dc.w	$4e7b,$0004
	move.l	_mm_orig+R_ITT1,d0
	dc.w	$4e7b,$0005
	move.l	_mm_orig+R_TT0,d0
	dc.w	$4e7b,$0006
	move.l	_mm_orig+R_TT1,d0
	dc.w	$4e7b,$0007
	dc.w	$f518			; pflusha
	move.l	_mm_orig+R_TC,d0
	dc.w	$4e7b,$0003
	move.l	_mm_orig+R_CACR,d0
	dc.w	$4e7b,$0002		; movec d0,cacr
	nop
	move.w	(sp)+,sr
	rts

; snippet for mm_run (MMU left off): a0 -> CRP value, a1 -> SRP value
_sn_load_rp30:
	dc.w	$f010,$4c00		; pmove (a0),crp
	dc.w	$f011,$4800		; pmove (a1),srp
	rts

; ------------------------------------------------------------------ helpers
_mm_nop:
	nop
	rts

_mm_pflusha:
	cmp.w	#3,_mm_fam
	bne.s	.f4
	dc.w	$f000,$2400		; pflusha (030)
	rts
.f4:	nop
	dc.w	$f518			; pflusha (040/060)
	nop
	rts

; ULONG p30_ptest(ULONG addr, ULONG fc, ULONG kind)
;   kind: bit 0 = write (PTESTW), bits 1-3 = level 0..7 (level > 0 also
;   returns the last descriptor address in p30_an through A1).
;   Returns MMUSR (16 bits).
PT30	macro				; \1 = level, \2 = R/W bit (1 = read)
	dc.w	$f010,$8000|(\1<<10)|(\2<<9)|$0100|(1<<5)|$08|1
	bra.w	pt30_out
	endm
PT30Z	macro				; level 0: no address register
	dc.w	$f010,$8000|(\2<<9)|$08|1
	bra.w	pt30_out
	endm
_p30_ptest:
	move.l	4(sp),a0
	move.l	8(sp),d1
	move.l	12(sp),d0
	sub.l	a1,a1
	lsl.w	#3,d0			; 8 bytes per entry
	jmp	pt30_tab(pc,d0.w)
pt30_tab:
	PT30Z	0,1			; kind 0: ptestr #0
	PT30Z	0,0			; kind 1: ptestw #0
	PT30	1,1
	PT30	1,0
	PT30	2,1
	PT30	2,0
	PT30	3,1
	PT30	3,0
	PT30	4,1
	PT30	4,0
	PT30	5,1
	PT30	5,0
	PT30	6,1
	PT30	6,0
	PT30	7,1
	PT30	7,0
pt30_out:
	move.l	a1,_p30_an
	lea	mm_w,a0
	dc.w	$f010,$6200		; pmove mmusr,(a0)
	moveq	#0,d0
	move.w	mm_w,d0
	rts

; void p30_pload(ULONG addr, ULONG fc, ULONG write)
_p30_pload:
	move.l	4(sp),a0
	move.l	8(sp),d1
	tst.l	12(sp)
	bne.s	.w
	dc.w	$f010,$2200|$08|1	; ploadr d1,(a0)
	rts
.w:	dc.w	$f010,$2000|$08|1	; ploadw d1,(a0)
	rts

; void p30_pflush(ULONG kind, ULONG fc, ULONG addr)
;   0 = PFLUSHA, 1 = PFLUSH d1,#7, 2 = PFLUSH d1,#4, 3 = PFLUSH d1,#7,(a0),
;   4 = PFLUSH d1,#0 (mask 0: every entry)
_p30_pflush:
	move.l	4(sp),d0
	move.l	8(sp),d1
	move.l	12(sp),a0
	tst.l	d0
	bne.s	.k1
	dc.w	$f000,$2400		; pflusha
	rts
.k1:	subq.l	#1,d0
	bne.s	.k2
	dc.w	$f000,$3000|(7<<5)|$08|1	; pflush d1,#7
	rts
.k2:	subq.l	#1,d0
	bne.s	.k3
	dc.w	$f000,$3000|(4<<5)|$08|1	; pflush d1,#4
	rts
.k3:	subq.l	#1,d0
	bne.s	.k4
	dc.w	$f010,$3800|(7<<5)|$08|1	; pflush d1,#7,(a0)
	rts
.k4:	dc.w	$f000,$3000|(0<<5)|$08|1	; pflush d1,#0
	rts

; void p30_dclr(void): clear the 030 data cache (CACR CD)
_p30_dclr:
	dc.w	$4e7a,$0002
	or.w	#$0800,d0
	dc.w	$4e7b,$0002
	rts

; ULONG p40_ptest(ULONG addr, ULONG fc, ULONG write): DFC = fc, PTESTR/W
; (a0), returns MMUSR. 68040 only.
_p40_ptest:
	move.l	4(sp),a0
	move.l	8(sp),d0
	dc.w	$4e7a,$1001		; movec dfc,d1
	dc.w	$4e7b,$0001		; movec d0,dfc
	tst.l	12(sp)
	bne.s	.w
	dc.w	$f568			; ptestr (a0)
	bra.s	.o
.w:	dc.w	$f548			; ptestw (a0)
.o:	dc.w	$4e7b,$1001		; movec d1,dfc
	dc.w	$4e7a,$0805		; movec mmusr,d0
	rts

; void p40_pflush(ULONG kind, ULONG fc, ULONG addr) (040/060)
;   0 = PFLUSHA, 1 = PFLUSHAN, 2 = PFLUSH (a0), 3 = PFLUSHN (a0); DFC = fc
_p40_pflush:
	move.l	4(sp),d0
	move.l	8(sp),d1
	move.l	12(sp),a0
	move.l	d2,-(sp)
	dc.w	$4e7a,$2001		; movec dfc,d2
	dc.w	$4e7b,$1001		; movec d1,dfc
	nop
	tst.l	d0
	bne.s	.k1
	dc.w	$f518			; pflusha
	bra.s	.o
.k1:	subq.l	#1,d0
	bne.s	.k2
	dc.w	$f510			; pflushan
	bra.s	.o
.k2:	subq.l	#1,d0
	bne.s	.k3
	dc.w	$f508			; pflush (a0)
	bra.s	.o
.k3:	dc.w	$f500			; pflushn (a0)
.o:	nop
	dc.w	$4e7b,$2001		; movec d2,dfc
	move.l	(sp)+,d2
	rts

; ULONG p60_plpa(ULONG addr, ULONG fc, ULONG write): DFC = fc, PLPAR/W (a0)
_p60_plpa:
	move.l	4(sp),a0
	move.l	8(sp),d0
	dc.w	$4e7a,$1001		; movec dfc,d1
	dc.w	$4e7b,$0001		; movec d0,dfc
	tst.l	12(sp)
	bne.s	.w
	dc.w	$f5c8			; plpar (a0)
	bra.s	.o
.w:	dc.w	$f588			; plpaw (a0)
.o:	dc.w	$4e7b,$1001		; movec d1,dfc
	move.l	a0,d0
	rts

_p40_cpusha_dc:
	nop
	dc.w	$f478			; cpusha dc
	cmp.w	#6,_mm_fam
	bne.s	.n
	dc.w	$f458			; 060: cinva dc (CACR.DPI=1: CPUSH keeps the lines)
.n:	nop
	rts
; void p40_cinvl_dc(ULONG physaddr)
_p40_cinvl_dc:
	move.l	4(sp),a0
	nop
	dc.w	$f448			; cinvl dc,(a0)
	nop
	rts
; void p40_cpushl_dc(ULONG physaddr)
_p40_cpushl_dc:
	move.l	4(sp),a0
	nop
	dc.w	$f468			; cpushl dc,(a0)
	nop
	rts

; ULONG mm_moves_get(ULONG addr, ULONG fc) (long)
_mm_moves_get:
	move.l	4(sp),a0
	move.l	8(sp),d0
	dc.w	$4e7a,$1000		; movec sfc,d1
	dc.w	$4e7b,$0000		; movec d0,sfc
	dc.w	$0e90,$0000		; moves.l (a0),d0
	dc.w	$4e7b,$1000		; movec d1,sfc
	rts
; void mm_moves_put(ULONG addr, ULONG val, ULONG fc, ULONG size)
;   size: 0 long, 1 byte, 2 word (the 040 SIZE encoding)
_mm_moves_put:
	move.l	4(sp),a0
	move.l	12(sp),d0
	move.l	d2,-(sp)
	dc.w	$4e7a,$2001		; movec dfc,d2
	dc.w	$4e7b,$0001		; movec d0,dfc
	move.l	4+8(sp),d1		; val
	move.l	4+16(sp),d0		; size
	bne.s	.nl
	dc.w	$0e90,$1800		; moves.l d1,(a0)
	bra.s	.o
.nl:	subq.l	#1,d0
	bne.s	.w
	dc.w	$0e10,$1800		; moves.b d1,(a0)
	bra.s	.o
.w:	dc.w	$0e50,$1800		; moves.w d1,(a0)
.o:	nop
	dc.w	$4e7b,$2001		; movec d2,dfc
	move.l	(sp)+,d2
	rts

; ------------------------------------------------------------------ snippets
; Inputs in registers (mm_in), _at = the instruction under test.
_sn_rd:
_sn_rd_at:
	move.l	(a0),d0
	rts
_sn_wr:
_sn_wr_at:
	move.l	d1,(a0)
	rts
_sn_addq:
_sn_addq_at:
	addq.l	#1,(a0)
	rts
_sn_rdpi:
_sn_rdpi_at:
	move.l	(a0)+,d0
	rts
_sn_wrpi:
_sn_wrpi_at:
	move.l	d1,(a0)+
	rts
_sn_tas:
_sn_tas_at:
	dc.w	$4ad0			; tas (a0)
	rts
; user mode: USP = mm_usp, then the access, then TRAP #0 (vector 32) ends
_sn_urd:
	move.l	_mm_usp,a6
	move.l	a6,usp
	andi.w	#$dfff,sr
_sn_urd_at:
	move.l	(a0),d0
	trap	#0
_sn_uwr:
	move.l	_mm_usp,a6
	move.l	a6,usp
	andi.w	#$dfff,sr
_sn_uwr_at:
	move.l	d1,(a0)
	trap	#0
; moves.l d1,(a0) with DFC = d2
_sn_moves_wr:
	dc.w	$4e7b,$2001		; movec d2,dfc
_sn_moves_wr_at:
	dc.w	$0e90,$1800		; moves.l d1,(a0)
	nop
	rts
; 060 PLPAR/PLPAW (a0) with DFC = d2; d0 = a0 after
_sn_plpar:
	dc.w	$4e7b,$2001		; movec d2,dfc
_sn_plpar_at:
	dc.w	$f5c8			; plpar (a0)
	move.l	a0,d0
	rts
_sn_plpaw:
	dc.w	$4e7b,$2001
_sn_plpaw_at:
	dc.w	$f588			; plpaw (a0)
	move.l	a0,d0
	rts
; 030 configuration exceptions (run with the MMU left off)
;   cfg_rp: pmove (a0),crp with a0 -> a root pointer with DT=0
;   cfg_tc: pmove (a1),tt0 (transparent for everything), then pmove (a0),tc
;           with a0 -> an enabled TC whose fields do not add up to 32
_sn_cfg_rp:
_sn_cfg_rp_at:
	dc.w	$f010,$4c00		; pmove (a0),crp
	nop
	rts
_sn_cfg_tc:
	dc.w	$f011,$0800		; pmove (a1),tt0
_sn_cfg_tc_at:
	dc.w	$f010,$4000		; pmove (a0),tc
	nop
	rts

; ------------------------------------------------------------------ stubs
MSTUB	macro
mstub_\1:
	move.w	sr,mm_hsr_tmp
	move.w	#\1,mm_cur
	bra.w	mm_common
	endm

	MSTUB	2
	MSTUB	3
	MSTUB	4
	MSTUB	5
	MSTUB	6
	MSTUB	7
	MSTUB	8
	MSTUB	9
	MSTUB	10
	MSTUB	11
	MSTUB	12
	MSTUB	13
	MSTUB	14
	MSTUB	15
	MSTUB	16
	MSTUB	17
	MSTUB	18
	MSTUB	19
	MSTUB	20
	MSTUB	21
	MSTUB	22
	MSTUB	23
	MSTUB	24
	MSTUB	25
	MSTUB	26
	MSTUB	27
	MSTUB	28
	MSTUB	29
	MSTUB	30
	MSTUB	31
	MSTUB	32
	MSTUB	33
	MSTUB	34
	MSTUB	35
	MSTUB	36
	MSTUB	37
	MSTUB	38
	MSTUB	39
	MSTUB	40
	MSTUB	41
	MSTUB	42
	MSTUB	43
	MSTUB	44
	MSTUB	45
	MSTUB	46
	MSTUB	47
	MSTUB	48
	MSTUB	49
	MSTUB	50
	MSTUB	51
	MSTUB	52
	MSTUB	53
	MSTUB	54
	MSTUB	55
	MSTUB	56
	MSTUB	57
	MSTUB	58
	MSTUB	59
	MSTUB	60
	MSTUB	61
	MSTUB	62
	MSTUB	63

mm_common:
	tst.w	_mm_active
	bne.s	.act
	subq.l	#4,sp			; chain to the previous handler
	move.l	a0,-(sp)
	move.w	mm_cur,a0
	add.l	a0,a0
	add.l	a0,a0
	add.l	#_mm_old,a0
	move.l	(a0),4(sp)
	move.l	(sp)+,a0
	rts
.act:	move.w	mm_cur,-(sp)		; this exception's vector
	movem.l	d0-d7/a0-a6,-(sp)	; 60 bytes; frame at 62(sp)
	move.w	_mm_nlog,d0
	cmp.w	#MLOGMAX,d0
	bhs	.runaway
	addq.w	#1,_mm_nlog
	mulu	#LOGSZ,d0
	lea	_mm_log,a0
	add.l	d0,a0
	move.w	60(sp),(a0)+		; vec
	move.w	mm_hsr_tmp,(a0)+	; SR the handler started with
	lea	62(sp),a1
	move.l	a1,(a0)+		; SSP = frame address
	moveq	#45,d1
.cp:	move.w	(a1)+,(a0)+
	dbf	d1,.cp
	move.l	_mm_hook,d1
	beq.s	.abort
	move.l	d1,a0
	moveq	#0,d1
	move.w	_mm_nlog,d1
	subq.l	#1,d1
	move.l	d1,-(sp)
	jsr	(a0)
	addq.l	#4,sp
	tst.l	d0
	beq.s	.abort
	movem.l	(sp)+,d0-d7/a0-a6
	addq.l	#2,sp
	rte
.abort:	lea	_mm_out,a0
	move.l	sp,a1
	moveq	#14,d0
.cp2:	move.l	(a1)+,(a0)+
	dbf	d0,.cp2
	move.w	60(sp),_mm_vec
	bra	mm_cont
.runaway:
	move.w	#$ffff,_mm_vec
	bra	mm_cont

; ------------------------------------------------------------------ data
	section	.data,data
	cnop	0,4
_mm_stub_table:
	dc.l	mstub_2
	dc.l	mstub_3
	dc.l	mstub_4
	dc.l	mstub_5
	dc.l	mstub_6
	dc.l	mstub_7
	dc.l	mstub_8
	dc.l	mstub_9
	dc.l	mstub_10
	dc.l	mstub_11
	dc.l	mstub_12
	dc.l	mstub_13
	dc.l	mstub_14
	dc.l	mstub_15
	dc.l	mstub_16
	dc.l	mstub_17
	dc.l	mstub_18
	dc.l	mstub_19
	dc.l	mstub_20
	dc.l	mstub_21
	dc.l	mstub_22
	dc.l	mstub_23
	dc.l	mstub_24
	dc.l	mstub_25
	dc.l	mstub_26
	dc.l	mstub_27
	dc.l	mstub_28
	dc.l	mstub_29
	dc.l	mstub_30
	dc.l	mstub_31
	dc.l	mstub_32
	dc.l	mstub_33
	dc.l	mstub_34
	dc.l	mstub_35
	dc.l	mstub_36
	dc.l	mstub_37
	dc.l	mstub_38
	dc.l	mstub_39
	dc.l	mstub_40
	dc.l	mstub_41
	dc.l	mstub_42
	dc.l	mstub_43
	dc.l	mstub_44
	dc.l	mstub_45
	dc.l	mstub_46
	dc.l	mstub_47
	dc.l	mstub_48
	dc.l	mstub_49
	dc.l	mstub_50
	dc.l	mstub_51
	dc.l	mstub_52
	dc.l	mstub_53
	dc.l	mstub_54
	dc.l	mstub_55
	dc.l	mstub_56
	dc.l	mstub_57
	dc.l	mstub_58
	dc.l	mstub_59
	dc.l	mstub_60
	dc.l	mstub_61
	dc.l	mstub_62
	dc.l	mstub_63
mm_zero:	dc.l	0,0

	section	.bss,bss
	cnop	0,4
_mm_cfg:	ds.l	13
_mm_orig:	ds.l	13
_mm_now:	ds.l	13
_mm_in:		ds.l	15
_mm_out:	ds.l	15
_mm_old:	ds.l	64
_mm_hook:	ds.l	1
_mm_usp:	ds.l	1
_p30_an:	ds.l	1
mm_fn:		ds.l	1
mm_save_sp:	ds.l	1
mm_cacr_save:	ds.l	1
mm_usp_save:	ds.l	1
mm_sfc_save:	ds.l	1
mm_dfc_save:	ds.l	1
_mm_log:	ds.b	LOGSZ*MLOGMAX
_mm_fam:	ds.w	1
_mm_vec:	ds.w	1
_mm_nlog:	ds.w	1
_mm_active:	ds.w	1
mm_cur:		ds.w	1
mm_hsr_tmp:	ds.w	1
mm_w:		ds.w	2
