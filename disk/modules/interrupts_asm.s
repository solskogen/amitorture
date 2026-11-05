; interrupts module: handlers and exact-timing test sequences.
;
; Generic logging handlers _ir_h1.._ir_h6 (vectors 25..30). Each entry
; appends a 32-byte record to _ir_log:
;   +0  tag.w      level (entry) or level|$80 (exit)
;   +2  sr.w       SR at handler entry (read before any CCR change)
;   +4  ssr.w      stacked SR
;   +6  fmt.w      stacked format/vector word (68010+, else 0)
;   +8  spc.l      stacked PC
;   +12 intreqr.w  INTREQR at entry
;   +14 snap.w     INTREQR & INTENAR & this level's bits
;   +16 d0.l       D0 of the interrupted code
;   +20 icr.w      CIA-A ICR (level 2) / CIA-B ICR (level 6) read, else 0
;   +22 seq.w      running entry number
;   +24..31        0
; After logging, an optional one-shot trigger word for this level is
; written to INTREQ (followed by a window in which a higher level can
; nest), then the snapshot bits are cleared and an exit record written.
; Level 3 VERTB is not logged unless _ir_logvertb is set; it is always
; chained to the runtime's level-3 handler (_ir_old27) so the frame
; counter and the watchdog keep running.
;
; Every CIA-A ICR read in a handler checks for a keyboard byte (SP set
; with CRA SPMODE = 0, serial port in input mode). The read consumed the
; flag, so the handler sets _ir_kbd_pend and the C side gives the keyboard
; its handshake (cia_kbd_handshake, >= 85 us KDAT low) between cases; a
; handler cannot wait that long without changing the timing under test.

	xdef	_ir_h1,_ir_h2,_ir_h3,_ir_h4,_ir_h5,_ir_h6
	xdef	_ir_log,_ir_logp,_ir_trig,_ir_fmt,_ir_logvertb,_ir_old27
	xdef	_ir_vertb_n,_ir_phantom,_ir_seq
	xdef	_ir_snap2,_ir_snapregs,_ir_snapsr,_ir_snappc,_ir_snapdone
	xdef	_ir_do_stop2000,_ir_after_stop2000
	xdef	_ir_do_stop2100,_ir_after_stop2100
	xdef	_ir_do_movesr,_ir_after_movesr
	xdef	_ir_do_andisr,_ir_after_andisr
	xdef	_ir_do_ccr,_ir_after_ccr
	xdef	_ir_do_chain,_ir_chain_start,_ir_chain_end,_ir_chain_t1,_ir_chain_t2,_ir_chain_t3
	xdef	_ir_do_movem,_ir_movem_start,_ir_movem_end,_ir_movem_t1,_ir_movem_t2
	xdef	_ir_st1,_ir_st2,_ir_st3,_ir_st_soft,_ir_st_cia,_ir_st_l2,_ir_st_vb
	xdef	_ir_lat2,_ir_lat3,_ir_lat_n,_ir_lat_buf,_ir_lat_overflow
	xdef	_ir_settle,_ir_st_other,_ir_runaway,_ir_kbd_pend

INTREQR	equ	$dff01e
INTENAR	equ	$dff01c
INTREQ	equ	$dff09c
INTENA	equ	$dff09a
VHPOSR	equ	$dff006
CIAA_ICR equ	$bfed01
CIAA_CRA equ	$bfee01
CIAA_TALO equ	$bfe401
CIAA_TAHI equ	$bfe501
CIAB_ICR equ	$bfdd00

LOGN	equ	64

; keyboard byte check after a CIA-A ICR read whose value is in \1 (a data
; register or a memory operand that may be read again); no register used
KBDCHK	macro
	btst	#3,\1
	beq.s	.k\@
	btst	#6,CIAA_CRA
	bne.s	.k\@
	st	_ir_kbd_pend
.k\@:
	endm

	section	.text,code

IRH	macro
_ir_h\1:
	movem.l	d0-d3/a0-a1,-(sp)
	move.w	sr,d2
	moveq	#\1,d1
	bra	ir_common
	endm

	IRH	1
	IRH	2
	IRH	3
	IRH	4
	IRH	5
	IRH	6

; stack after the push of the snapshot:
;   0 snap.w, 2 d0, 6 d1, 10 d2, 14 d3, 18 a0, 22 a1, 26 SR, 28 PC, 32 fmt
ir_common:
	move.w	INTREQR,d0
	and.w	INTENAR,d0
	move.w	d1,d3
	add.w	d3,d3
	lea	ir_lvlbits(pc),a0
	and.w	0(a0,d3.w),d0
	move.w	d0,-(sp)
	cmp.w	#3,d1
	bne.s	.log
	btst	#5,d0
	beq.s	.log
	addq.l	#1,_ir_vertb_n
	tst.w	_ir_logvertb
	bne.s	.log
	and.w	#$ffdf,d0
	beq	.exit			; VERTB only: chain
.log:	tst.w	d0
	bne.s	.real
	addq.l	#1,_ir_phantom		; nothing pending for this level
	bra	.exit
.real:	move.l	_ir_logp,a1
	cmp.l	#ir_log_end,a1
	bhs.s	.nolog1
	move.w	d1,(a1)+
	move.w	d2,(a1)+
	move.w	26(sp),(a1)+
	clr.w	(a1)
	tst.w	_ir_fmt
	beq.s	.nofmt
	move.w	32(sp),(a1)
.nofmt:	addq.l	#2,a1
	move.l	28(sp),(a1)+
	move.w	INTREQR,(a1)+
	move.w	(sp),(a1)+
	move.l	2(sp),(a1)+
	clr.w	(a1)
	cmp.w	#2,d1
	bne.s	.n2
	move.b	CIAA_ICR,1(a1)
	KBDCHK	1(a1)
.n2:	cmp.w	#6,d1
	bne.s	.n6
	move.b	CIAB_ICR,1(a1)
.n6:	addq.l	#2,a1
	addq.w	#1,_ir_seq
	move.w	_ir_seq,(a1)+
	clr.l	(a1)+
	clr.l	(a1)+
	move.l	a1,_ir_logp
	bra.s	.trig
.nolog1:
	; log full: a source that will not clear (runaway). Disable its
	; INTENA bits (never VERTB) so every case ends in bounded time.
	move.w	(sp),d0
	and.w	#$ffdf,d0
	move.w	d0,INTENA
	addq.l	#1,_ir_runaway
	cmp.w	#2,d1			; still release the CIA lines
	bne.s	.m2
	move.b	CIAA_ICR,d0
	KBDCHK	d0
.m2:	cmp.w	#6,d1
	bne.s	.trig
	tst.b	CIAB_ICR
.trig:	lea	_ir_trig,a0
	move.w	0(a0,d3.w),d0
	beq.s	.notrig
	clr.w	0(a0,d3.w)
	move.w	d0,INTREQ
	; window for a nested interrupt: two beam line changes (64-128 us),
	; bounded by 65536 polls if the beam stands still. Bounded by beam
	; time, not by a read count, so a core with coarser interrupt
	; delivery still gets the request in. d2/d3 are free here.
	move.b	VHPOSR,d3
	moveq	#2,d2
	moveq	#-1,d0
.win:	cmp.b	VHPOSR,d3
	beq.s	.wn
	move.b	VHPOSR,d3
	subq.w	#1,d2
	beq.s	.notrig
.wn:	dbf	d0,.win
.notrig:
	move.w	(sp),d0
	cmp.w	#3,d1
	bne.s	.clr
	and.w	#$ffdf,d0		; VERTB is cleared by the runtime
.clr:	move.w	d0,INTREQ
	move.w	d0,INTREQ
	tst.w	INTREQR
	move.l	_ir_logp,a1
	cmp.l	#ir_log_end,a1
	bhs.s	.exit
	move.w	d1,d0
	or.w	#$80,d0
	move.w	d0,(a1)+
	move.w	sr,(a1)+
	moveq	#6,d0
.z:	clr.l	(a1)+
	dbf	d0,.z
	move.l	a1,_ir_logp
.exit:	move.w	(sp)+,d0
	cmp.w	#3,d1
	bne.s	.rte
	btst	#5,d0
	beq.s	.rte
	movem.l	(sp)+,d0-d3/a0-a1
	move.l	_ir_old27,-(sp)
	rts				; into the runtime handler, frame intact
.rte:	movem.l	(sp)+,d0-d3/a0-a1
	rte

ir_lvlbits:
	dc.w	$0000,$0007,$0008,$0070,$0780,$1800,$2000,$0000

; a few slow chip reads so a request written to INTREQ reaches the IPL lines
_ir_settle:
	tst.w	INTREQR
	tst.w	INTREQR
	tst.w	INTREQR
	tst.w	INTREQR
	tst.w	INTREQR
	tst.w	INTREQR
	tst.w	INTREQR
	tst.w	INTREQR
	rts

; ---------------------------------------------------------------- snapshot
; level 2 (CIA-A) handler: all registers, stacked SR/PC, then ack
_ir_snap2:
	movem.l	d0-d7/a0-a6,_ir_snapregs
	move.w	(sp),_ir_snapsr
	move.l	2(sp),_ir_snappc
	move.w	#$0008,INTENA		; one entry per arming (C re-enables)
	move.b	CIAA_ICR,_ir_icr_tmp
	KBDCHK	_ir_icr_tmp
	move.w	#$0008,INTREQ
	move.w	#$0008,INTREQ
	tst.w	INTREQR
	move.w	#1,_ir_snapdone
	rte

; ---------------------------------------------------------------- STOP
_ir_do_stop2000:
	stop	#$2000
_ir_after_stop2000:
	rts

_ir_do_stop2100:
	stop	#$2100
_ir_after_stop2100:
	rts

; ---------------------------------------------------------------- mask lowering
; SOFTINT requested with the mask at 7, then the mask lowered by one
; instruction: the interrupt must be taken before the next instruction.
_ir_do_movesr:
	move.w	#$2700,sr
	move.w	#$8004,INTREQ
	bsr	_ir_settle
	move.w	#$2000,sr
_ir_after_movesr:
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	rts

_ir_do_andisr:
	move.w	#$2700,sr
	move.w	#$8004,INTREQ
	bsr	_ir_settle
	andi.w	#$f8ff,sr
_ir_after_andisr:
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	rts

; AUD0 (level 4) with SR=$2315 (mask 3, CCR X/Z/C): stacked SR must be
; $2315, the handler's SR $2415 (CCR unchanged)
_ir_do_ccr:
	move.w	#$2700,sr
	move.w	#$8080,INTREQ
	bsr	_ir_settle
	move.w	#$2315,sr
_ir_after_ccr:
	nop
	nop
	nop
	nop
	move.w	#$2000,sr
	rts

; ---------------------------------------------------------------- chains
; ADDQ chain: at an interrupt, D0 == number of ADDQs executed, so
; D0 == (PC - start) / 2 inside the chain. Loops until _ir_snapdone.
_ir_do_chain:
	moveq	#0,d0
_ir_chain_start:
	rept	2000
	addq.l	#1,d0
	endr
_ir_chain_end:
	tst.w	_ir_snapdone
_ir_chain_t1:
	bne.s	ir_chain_out
_ir_chain_t2:
	moveq	#0,d0
_ir_chain_t3:
	bra	_ir_chain_start
ir_chain_out:
	rts

; MOVEM chain: alternately all of d0-d7/a2-a6 from buffer A and buffer B.
; An interrupt must see the registers all from one buffer: the one of the
; last MOVEM before the stacked PC. void ir_do_movem(ULONG *a, ULONG *b)
_ir_do_movem:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0
	move.l	52(sp),a1
	movem.l	(a1),d0-d7/a2-a6
_ir_movem_start:
	rept	128
	movem.l	(a0),d0-d7/a2-a6
	movem.l	(a1),d0-d7/a2-a6
	endr
_ir_movem_end:
	tst.w	_ir_snapdone
_ir_movem_t1:
	bne.s	ir_movem_out
_ir_movem_t2:
	bra	_ir_movem_start
ir_movem_out:
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; ---------------------------------------------------------------- storm
_ir_st1:				; SOFTINT
	move.w	#$0004,INTREQ
	move.w	#$0004,INTREQ
	addq.l	#1,_ir_st_soft
	tst.w	INTREQR
	rte

_ir_st2:				; CIA-A
	move.l	d0,-(sp)
	move.b	CIAA_ICR,d0
	KBDCHK	d0
	move.w	#$0008,INTREQ
	move.w	#$0008,INTREQ
	addq.l	#1,_ir_st_l2
	btst	#0,d0
	beq.s	.n
	addq.l	#1,_ir_st_cia
	bra.s	.o
.n:	addq.l	#1,_ir_st_other		; level 2 without a CIA-A TA flag
	cmp.l	#4096,_ir_st_other
	blo.s	.o
	move.w	#$0008,INTENA		; runaway: stop it, the counts will show it
.o:	tst.w	INTREQR
	move.l	(sp)+,d0
	rte

_ir_st3:				; VERTB: count, then the runtime handler
	btst	#5,INTREQR+1
	beq.s	.c
	addq.l	#1,_ir_st_vb
.c:	move.l	_ir_old27,-(sp)
	rts

; ---------------------------------------------------------------- latency
; level 2: CIA-A timer A (continuous) counter at entry, then stop it
_ir_lat2:
	move.l	a0,-(sp)
	cmp.w	#64,_ir_lat_n
	bhs.s	.full
	move.l	_ir_lat_p,a0
	move.b	CIAA_TAHI,(a0)+
	move.b	CIAA_TALO,(a0)+
	move.b	CIAA_TAHI,(a0)+
	move.b	#0,CIAA_CRA
	move.b	CIAA_ICR,(a0)+
	KBDCHK	-1(a0)
	move.l	a0,_ir_lat_p
	move.w	#$0008,INTENA		; one entry per arming (C re-enables)
	move.w	#$0008,INTREQ
	move.w	#$0008,INTREQ
	addq.w	#1,_ir_lat_n
	tst.w	INTREQR
	move.l	(sp)+,a0
	rte
.full:	move.w	#1,_ir_lat_overflow
	move.w	#$0008,INTENA
	move.w	#$0008,INTREQ
	move.l	(sp)+,a0
	rte

; level 3: COPER from the copper: beam position at entry
_ir_lat3:
	move.l	a0,-(sp)
	btst	#4,INTREQR+1
	beq.s	.nc
	cmp.w	#64,_ir_lat_n
	bhs.s	.full
	move.l	_ir_lat_p,a0
	move.w	VHPOSR,(a0)
	addq.l	#4,a0
	move.l	a0,_ir_lat_p
	addq.w	#1,_ir_lat_n
	move.w	#$0010,INTREQ
	move.w	#$0010,INTREQ
	bra.s	.nc
.full:	move.w	#1,_ir_lat_overflow
	move.w	#$0010,INTENA
	move.w	#$0010,INTREQ
	move.w	#$0010,INTREQ
.nc:	move.l	(sp)+,a0
	btst	#5,INTREQR+1
	beq.s	.r
	move.l	_ir_old27,-(sp)
	rts
.r:	tst.w	INTREQR
	rte

	section	.data,data
_ir_logp:	dc.l	_ir_log
_ir_lat_p:	dc.l	_ir_lat_buf
	xdef	_ir_lat_p

	section	.bss,bss
_ir_log:	ds.b	32*LOGN
ir_log_end:
_ir_trig:	ds.w	8
_ir_fmt:	ds.w	1
_ir_logvertb:	ds.w	1
_ir_seq:	ds.w	1
_ir_snapdone:	ds.w	1
_ir_snapsr:	ds.w	1
_ir_lat_n:	ds.w	1
_ir_lat_overflow: ds.w	1
_ir_old27:	ds.l	1
_ir_vertb_n:	ds.l	1
_ir_phantom:	ds.l	1
_ir_snappc:	ds.l	1
_ir_snapregs:	ds.l	15
_ir_st_soft:	ds.l	1
_ir_st_cia:	ds.l	1
_ir_st_l2:	ds.l	1
_ir_st_vb:	ds.l	1
_ir_st_other:	ds.l	1
_ir_runaway:	ds.l	1
_ir_kbd_pend:	ds.b	1
_ir_icr_tmp:	ds.b	1
_ir_lat_buf:	ds.l	64
