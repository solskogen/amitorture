; smc module: code templates (position independent; copied into the pools
; and patched there) and call helpers. 68000 ISA.

	section	.text,code

; ---- far routine: returns an immediate (patched at +2)
	xdef	_smc_tpl_ret
	xdef	_smc_tpl_ret_end
	cnop	0,4
_smc_tpl_ret:
	move.l	#$11111111,d0
	rts
_smc_tpl_ret_end:

; ---- self-patching routine: a0 = word to patch, d1 = new word.
; The writer is at +2 (one word); the next instruction is at +4, so
; distance D from the next instruction = a0 - (base + 4). All slots are
; NOPs; the patch word is MOVEQ #1,d0, so d0 = 1 if the new word ran.
	xdef	_smc_tpl_self
	xdef	_smc_tpl_self_end
	cnop	0,4
_smc_tpl_self:
	moveq	#0,d0
	move.w	d1,(a0)
	nop			; +4  D=0
	nop			; +6  D=2
	nop			; +8  D=4
	nop			; +10 D=6
	nop			; +12 D=8
	nop
	nop
	nop
	rts
_smc_tpl_self_end:

; ---- DBF loop whose single (loop-mode capable on the 68010) instruction
; overwrites itself: a0 = base, d1 = new word ($5242 addq.w #1,d2),
; d0 = count-1, d2 = 0. Returns d2: 0 if the old MOVE kept running,
; count-1 if the new ADDQ ran from the second pass on.
	xdef	_smc_tpl_loop
	xdef	_smc_tpl_loop_end
	cnop	0,4
_smc_tpl_loop:
	move.w	d1,(a0)
	dbf	d0,_smc_tpl_loop
	move.l	d2,d0
	rts
_smc_tpl_loop_end:

; ---- one patchable ALU opcode: d0 = a, d1 = b
	xdef	_smc_tpl_op
	xdef	_smc_tpl_op_end
	cnop	0,4
_smc_tpl_op:
	add.l	d1,d0
	rts
_smc_tpl_op_end:

; ---- patchable BRA.S (displacement byte at +1): 2, 6 or 10
	xdef	_smc_tpl_bra
	xdef	_smc_tpl_bra_end
	cnop	0,4
_smc_tpl_bra:
	bra.s	.t1		; +0
	nop			; +2
.t1:	moveq	#1,d0		; +4
	rts
.t2:	moveq	#2,d0		; +8
	rts
.t3:	moveq	#3,d0		; +12
	rts
_smc_tpl_bra_end:

; ---- call helpers (C ABI)
; ULONG smc_call0(void *code)
	xdef	_smc_call0
_smc_call0:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a1
	jsr	(a1)
	movem.l	(sp)+,d2-d7/a2-a6
	rts
; ULONG smc_call_self(void *code, void *patch, ULONG word)
	xdef	_smc_call_self
_smc_call_self:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a1
	move.l	52(sp),a0
	move.l	56(sp),d1
	jsr	(a1)
	movem.l	(sp)+,d2-d7/a2-a6
	rts
; ULONG smc_call_loop(void *code, ULONG word, ULONG count)
	xdef	_smc_call_loop
_smc_call_loop:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0
	move.l	52(sp),d1
	move.l	56(sp),d0
	subq.l	#1,d0
	moveq	#0,d2
	jsr	(a0)
	movem.l	(sp)+,d2-d7/a2-a6
	rts
; ULONG smc_call2(void *code, ULONG a, ULONG b)
	xdef	_smc_call2
_smc_call2:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0
	move.l	52(sp),d0
	move.l	56(sp),d1
	jsr	(a0)
	movem.l	(sp)+,d2-d7/a2-a6
	rts
