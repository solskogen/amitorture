; smc module: 68040/68060 cache maintenance. Only called on 040/060.
; Never invalidates the data cache wholesale (CINVA DC/BC would drop dirty
; copyback lines of the stack and BSS); CINVL DC is used only on lines the
; module owns completely.
	machine	68040
	section	.text,code
	xdef	_smc_cpusha_bc
	xdef	_smc_cpusha_dc
	xdef	_smc_cinva_ic
	xdef	_smc_cpushl_bc
	xdef	_smc_cpushl_dc
	xdef	_smc_cinvl_ic
	xdef	_smc_cinvl_dc
_smc_cpusha_bc:
	nop
	cpusha	bc
	nop
	rts
_smc_cpusha_dc:
	nop
	cpusha	dc
	nop
	rts
_smc_cinva_ic:
	nop
	cinva	ic
	nop
	rts
_smc_cpushl_bc:
	move.l	4(sp),a0
	nop
	cpushl	bc,(a0)
	nop
	rts
_smc_cpushl_dc:
	move.l	4(sp),a0
	nop
	cpushl	dc,(a0)
	nop
	rts
_smc_cinvl_ic:
	move.l	4(sp),a0
	nop
	cinvl	ic,(a0)
	nop
	rts
_smc_cinvl_dc:
	move.l	4(sp),a0
	nop
	cinvl	dc,(a0)
	nop
	rts
