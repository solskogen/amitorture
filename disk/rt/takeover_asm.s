; amitorture takeover helpers (68000 code)
;
; void amt_coldreboot_ks1(void)
;   KS1.x has no ColdReboot(): the standard sequence from the Amiga hardware
;   manual. RESET maps the ROM at 0; the JMP is already prefetched, so it
;   runs and jumps through the ROM's reset entry at address 2.

	xref	_SysBase
	xdef	_amt_coldreboot_ks1

_LVOSupervisor	equ	-30

	section	.text,code

_amt_coldreboot_ks1:
	move.l	_SysBase,a6
	lea	.magic(pc),a5
	jsr	_LVOSupervisor(a6)
	; not reached

	cnop	0,4
.magic:
	lea	2.w,a0
	reset
	jmp	(a0)
