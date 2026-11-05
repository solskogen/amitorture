; paula module: audio interrupt logger (level 4, vector 28) and helpers.
;
; _pa_l4 appends one 12-byte entry per interrupt to _pa_log:
;   +0  frames.l   runtime VBL count (_amt_frames) at entry
;   +4  vposr.w    VPOSR  \ read VPOSR, VHPOSR, VPOSR until the two
;   +6  vhposr.w   VHPOSR / VPOSR reads agree in V8-V10 (consistent beam)
;   +8  intreqr.w  INTREQR as read (VERTB pending tells a frame wrap the
;                  runtime has not counted yet)
;   +10 pend.w     INTREQR & INTENAR & $0780 (the audio channels this
;                  entry is for)
; Then, with n = ++_pa_n:
;   - n == _pa_stop_at: the pending bits are NOT cleared, their INTENA
;     bits are cleared instead (the request stays pending: AUDxIP = 1);
;   - otherwise the pending bits are cleared in INTREQ;
;   - up to 4 actions {n.w, addr.l, val.w} (_pa_act, 8 bytes each):
;     when n matches, val is written (word) to addr;
;   - if _pa_feed_left != 0 (and n != _pa_stop_at): the next word from
;     _pa_feed_p is written to _pa_feed_a (manual-mode AUDxDAT feed).
;
; _pa_stamp(ent *e): one entry for "now" (same format, pend = 0).
; _pa_stamp_write(ent *e, UWORD *reg, UWORD val): stamp, then write val to
;     reg at once (DMACON on / first AUDxDAT write of a timing test).

	xdef	_pa_l4
	xdef	_pa_log,_pa_logp,_pa_log_end,_pa_n,_pa_stop_at
	xdef	_pa_act,_pa_feed_left,_pa_feed_p,_pa_feed_a
	xdef	_pa_stamp,_pa_stamp_write
	xdef	_pa_sample_invalid
	xref	_amt_frames

PA_LOGN	equ	96

	section	.text,code

_pa_l4:
	movem.l	d0-d3/a0-a1,-(sp)
	move.w	#$ffff,d3		; retry bound: a cycle-exact CPU leaves the wrap
				; window in a retry or two; a fast core reads the beam
				; without chipset time passing (CPU bursts), so allow
				; enough reads for its burst to end
.t:	move.w	$dff004,d0
	move.w	$dff006,d1
	move.w	$dff004,d2
	eor.w	d0,d2
	and.w	#$0007,d2
	bne.s	.retry
	cmp.b	#3,d1			; near the line wrap V and H may not
	bcs.s	.retry			; change together: read again
	cmp.b	#224,d1
	bhi.s	.retry
	bra.s	.sample
.retry:	dbf	d3,.t
	move.w	#1,_pa_sample_invalid
	move.w	#$0780,$dff09a
	move.w	#$0780,$dff09c
	movem.l	(sp)+,d0-d3/a0-a1
	rte
.sample:
	move.w	$dff01e,d2
	move.l	_pa_logp,a0
	cmp.l	#_pa_log_end,a0
	bhs.s	.full
	move.l	_amt_frames,(a0)+
	move.w	d0,(a0)+
	move.w	d1,(a0)+
	move.w	d2,(a0)+
	and.w	$dff01c,d2
	and.w	#$0780,d2
	move.w	d2,(a0)+
	move.l	a0,_pa_logp
	bra.s	.go
.full:	and.w	$dff01c,d2
	and.w	#$0780,d2
.go:	addq.w	#1,_pa_n
	move.w	_pa_n,d0
	cmp.w	_pa_stop_at,d0
	bne.s	.clr
	move.w	d2,$dff09a		; INTENA: clear these sources, leave INTREQ set
	bra.s	.acts
.clr:	move.w	d2,$dff09c
	move.w	d2,$dff09c
.acts:	lea	_pa_act,a1
	moveq	#3,d1
.al:	cmp.w	(a1),d0
	bne.s	.an
	move.l	2(a1),a0
	move.w	6(a1),(a0)
.an:	addq.l	#8,a1
	dbf	d1,.al
	tst.w	_pa_feed_left
	beq.s	.nf
	cmp.w	_pa_stop_at,d0
	beq.s	.nf
	subq.w	#1,_pa_feed_left
	move.l	_pa_feed_p,a0
	move.l	_pa_feed_a,a1
	move.w	(a0)+,(a1)
	move.l	a0,_pa_feed_p
.nf:	movem.l	(sp)+,d0-d3/a0-a1
	rte

; void pa_stamp(ent *e)
_pa_stamp:
	move.l	4(sp),a0
	move.w	sr,-(sp)
	move.w	#$2700,sr
	bsr.s	stamp
	move.w	(sp)+,sr
	rts

; void pa_stamp_write(ent *e, volatile UWORD *reg, ULONG val)
_pa_stamp_write:
	move.l	4(sp),a0
	move.l	8(sp),a1
	move.l	12(sp),d1
	move.w	sr,-(sp)
	move.w	#$2700,sr
	movem.l	d1/a1,-(sp)
	bsr.s	stamp
	movem.l	(sp)+,d1/a1
	move.w	d1,(a1)
	move.w	(sp)+,sr
	rts

; a0 = entry; trashes d0/d1/d2 (d2 saved)
stamp:
	movem.l	d2-d3,-(sp)
	move.w	#$ffff,d3		; retry bound: a cycle-exact CPU leaves the wrap
				; window in a retry or two; a fast core reads the beam
				; without chipset time passing (CPU bursts), so allow
				; enough reads for its burst to end
.t:	move.w	$dff004,d0
	move.w	$dff006,d1
	move.w	$dff004,d2
	eor.w	d0,d2
	and.w	#$0007,d2
	bne.s	.retry
	cmp.b	#3,d1			; near the line wrap V and H may not
	bcs.s	.retry			; change together: read again
	cmp.b	#224,d1
	bhi.s	.retry
	bra.s	.sample
.retry:	dbf	d3,.t
	move.w	#1,_pa_sample_invalid
.sample:
	move.l	_amt_frames,(a0)+
	move.w	d0,(a0)+
	move.w	d1,(a0)+
	move.w	$dff01e,(a0)+
	clr.w	(a0)+
	movem.l	(sp)+,d2-d3
	rts

	section	.data,data
_pa_logp:	dc.l	_pa_log

	section	.bss,bss
_pa_log:	ds.b	12*PA_LOGN
_pa_log_end:
_pa_n:		ds.w	1
_pa_sample_invalid: ds.w	1
_pa_stop_at:	ds.w	1
_pa_feed_left:	ds.w	1
		ds.w	1
_pa_feed_p:	ds.l	1
_pa_feed_a:	ds.l	1
_pa_act:	ds.b	8*4
