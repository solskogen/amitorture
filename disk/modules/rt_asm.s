; rt module helpers
	xdef	_rt_illegal
	xdef	_rt_trap0
	xdef	_rt_trap0_handler
	xdef	_rt_trap_count
	section	.text,code
_rt_illegal:
	illegal
	rts
_rt_trap0:
	trap	#0
	rts
_rt_trap0_handler:
	addq.l	#1,_rt_trap_count
	rte
	section	.bss,bss
_rt_trap_count:
	ds.l	1
