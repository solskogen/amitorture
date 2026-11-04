; amitorture CPU probe primitives (CONTRACT.md section 8)
;
; ULONG amt_probe_sv(ULONG id, ULONG *io, ULONG vbr, ULONG install)
;
; Runs test `id` in supervisor mode (exec Supervisor()), with the interrupt
; mask at 7 and, if `install` is non-zero, our own handlers in the vector
; table at `vbr` for vectors 2-15 and 48-61.  Every exception in that set
; that the test raises is caught: the handler drops the exception frame
; (SP is reset to the value it had before the test was called) and resumes
; after the test.  The vectors are restored before returning.
;
; Returns 0 when the test ran to completion, else the vector number of the
; exception that ended it.  amt_probe_trappc holds the stacked PC of the
; last caught exception (meaningful for group 1/2 exceptions).
;
; The caller (probe.c) must hold Disable().  The tests use CPU-specific
; instructions (MOVEC, PMOVE, FPU ops); they are encoded with dc.w where the
; exact encoding matters, so that one object holds all of them and nothing
; depends on the assembler's CPU setting.  Only probe.c decides which test
; is safe to run on which CPU.
;
; Assembled with vasm (Motorola syntax); the file sets its own CPU type.

        machine 68060
        fpu     1
        opt     o-

        section .text,code

        xdef    _amt_probe_sv
        xdef    _amt_probe_trappc
        xref    _SysBase

; test ids (keep in sync with probe.c)
T_VBR           equ 0
NTESTS          equ 18

;----------------------------------------------------------------------------
_amt_probe_sv:
        movem.l d2-d7/a2-a6,-(sp)               ; 44 bytes
        movem.l 48(sp),d0-d3                    ; id, io, vbr, install
        lea     sv_args,a0
        movem.l d0-d3,(a0)
        clr.l   sv_trap
        clr.l   sv_cleanup
        move.l  _SysBase,a6
        lea     sv_entry(pc),a5
        jsr     -30(a6)                         ; Supervisor(a5)
        move.l  sv_trap,d0
        movem.l (sp)+,d2-d7/a2-a6
        rts

;----------------------------------------------------------------------------
; supervisor part; the exec frame is on the SSP, leave with RTE
sv_entry:
        or.w    #$0700,sr
        movem.l d0-d7/a0-a6,-(sp)
        lea     sv_args,a4
        tst.l   12(a4)
        beq.s   sv_noinst
        bsr     vec_install
sv_noinst:
        move.l  (a4),d0
        cmp.l   #NTESTS,d0
        bcc.s   sv_badid
        add.w   d0,d0
        add.w   d0,d0
        lea     test_table(pc),a1
        move.l  0(a1,d0.w),a1
        move.l  4(a4),a0                        ; io
        move.l  sp,sv_spsave
        jsr     (a1)
sv_resume:
        lea     sv_args,a4
        move.l  sv_cleanup,d0
        beq.s   sv_noclean
        clr.l   sv_cleanup
        move.l  d0,a1
        move.l  4(a4),a0
        jsr     (a1)
sv_noclean:
        tst.l   12(a4)
        beq.s   sv_norest
        bsr     vec_restore
sv_norest:
        movem.l (sp)+,d0-d7/a0-a6
        rte
sv_badid:
        move.l  #-1,sv_trap
        bra.s   sv_noclean

;----------------------------------------------------------------------------
; common trap path: the stub has stored the vector number
trap_common:
        move.l  2(sp),_amt_probe_trappc
        move.l  sv_spsave,sp
        bra.s   sv_resume

TSTUB   macro
stub\1: move.w  #\1,sv_trap+2
        bra.w   trap_common
        endm

        TSTUB   2
        TSTUB   3
        TSTUB   4
        TSTUB   5
        TSTUB   6
        TSTUB   7
        TSTUB   8
        TSTUB   9
        TSTUB   10
        TSTUB   11
        TSTUB   12
        TSTUB   13
        TSTUB   14
        TSTUB   15
        TSTUB   48
        TSTUB   49
        TSTUB   50
        TSTUB   51
        TSTUB   52
        TSTUB   53
        TSTUB   54
        TSTUB   55
        TSTUB   56
        TSTUB   57
        TSTUB   58
        TSTUB   59
        TSTUB   60
        TSTUB   61

NVEC    equ     28

        cnop    0,4
vec_list:
        dc.l    2,stub2,3,stub3,4,stub4,5,stub5,6,stub6,7,stub7
        dc.l    8,stub8,9,stub9,10,stub10,11,stub11,12,stub12
        dc.l    13,stub13,14,stub14,15,stub15
        dc.l    48,stub48,49,stub49,50,stub50,51,stub51,52,stub52
        dc.l    53,stub53,54,stub54,55,stub55,56,stub56,57,stub57
        dc.l    58,stub58,59,stub59,60,stub60,61,stub61

; a4 = sv_args
vec_install:
        move.l  8(a4),a1
        lea     vec_list(pc),a2
        lea     vec_save,a3
        moveq   #NVEC-1,d2
.l:     move.l  (a2)+,d0
        move.l  (a2)+,d1
        lsl.w   #2,d0
        move.l  0(a1,d0.w),(a3)+
        move.l  d1,0(a1,d0.w)
        dbf     d2,.l
        rts

vec_restore:
        move.l  8(a4),a1
        lea     vec_list(pc),a2
        lea     vec_save,a3
        moveq   #NVEC-1,d2
.l:     move.l  (a2)+,d0
        addq.l  #4,a2
        lsl.w   #2,d0
        move.l  (a3)+,0(a1,d0.w)
        dbf     d2,.l
        rts

;----------------------------------------------------------------------------
; tests: a0 = io, must preserve a4; may use d0-d7/a0-a3/a5/a6
        cnop    0,4
test_table:
        dc.l    t_vbr           ; 0  MOVEC VBR,D0               -> io[0]
        dc.l    t_moveccr       ; 1  MOVE CCR,D0 (010+)         -> io[0]
        dc.l    t_cacr          ; 2  MOVEC CACR,D0              -> io[0]
        dc.l    t_scale         ; 3  LEA (0,A1,D0.W*4) d0=1      -> io[0]
        dc.l    t_cacrw         ; 4  CACR := io[0]|io[1], read  -> io[2], restore io[0]
        dc.l    t_pcr           ; 5  MOVEC PCR,D0 (060)         -> io[0]
        dc.l    t_pcrw          ; 6  PCR := io[1], read back    -> io[2]
        dc.l    t_itt0          ; 7  MOVEC ITT0/DTT0 (040/060)  -> io[0], io[1]
        dc.l    t_caar          ; 8  MOVEC CAAR (020/030)       -> io[0]
        dc.l    t_tc4           ; 9  MOVEC TC/URP/SRP (040/060) -> io[0..2]
        dc.l    t_tc4w          ; 10 TC/URP write/read/restore  (see below)
        dc.l    t_pmove_rd      ; 11 PMOVE TC,(io)              -> io[0]
        dc.l    t_pmove_tt      ; 12 PMOVE TT0/TT1 (030)        -> io[0], io[1]
        dc.l    t_pmove_w       ; 13 PMOVE TC write/read/restore
        dc.l    t_mmu030u       ; 14 030 table walk (U bit) test
        dc.l    t_fpu           ; 15 FMOVE.L FPCR,D0            -> io[0]
        dc.l    t_fsave         ; 16 FSAVE/FRESTORE             -> io[0..], io[64..]
        dc.l    t_nop           ; 17 nothing (vector install/restore only)

t_nop:
        rts

t_vbr:
        dc.w    $4e7a,$0801             ; movec vbr,d0
        move.l  d0,(a0)
        rts

t_moveccr:
        moveq   #0,d0
        dc.w    $42c0                   ; move ccr,d0 (illegal on the 68000)
        move.l  d0,(a0)
        rts

t_cacr:
        dc.w    $4e7a,$0002             ; movec cacr,d0
        move.l  d0,(a0)
        rts

t_scale:
        moveq   #1,d0
        sub.l   a1,a1
        dc.w    $43f1,$0400             ; lea (0,a1,d0.w*4),a1 (scale ignored before 020)
        move.l  a1,(a0)
        rts

; io[0] = original CACR, io[1] = bits to set; io[2] <- read back
t_cacrw:
        move.l  (a0),d0
        move.l  d0,d1
        or.l    4(a0),d1
        dc.w    $4e7b,$1002             ; movec d1,cacr
        dc.w    $4e7a,$2002             ; movec cacr,d2
        dc.w    $4e7b,$0002             ; movec d0,cacr
        move.l  d2,8(a0)
        rts

t_pcr:
        dc.w    $4e7a,$0808             ; movec pcr,d0
        move.l  d0,(a0)
        rts

; io[1] = new PCR; io[2] <- read back (no restore: probe.c restores it)
t_pcrw:
        move.l  4(a0),d0
        dc.w    $4e7b,$0808             ; movec d0,pcr
        dc.w    $4e7a,$1808             ; movec pcr,d1
        move.l  d1,8(a0)
        rts

t_itt0:
        dc.w    $4e7a,$0004             ; movec itt0,d0
        move.l  d0,(a0)
        dc.w    $4e7a,$0006             ; movec dtt0,d0
        move.l  d0,4(a0)
        rts

t_caar:
        dc.w    $4e7a,$0802             ; movec caar,d0
        move.l  d0,(a0)
        rts

t_tc4:
        dc.w    $4e7a,$0003             ; movec tc,d0
        move.l  d0,(a0)
        dc.w    $4e7a,$0806             ; movec urp,d0
        move.l  d0,4(a0)
        dc.w    $4e7a,$0807             ; movec srp,d0
        move.l  d0,8(a0)
        rts

; io[0] orig TC, io[1] test TC, io[2] <- TC read back
; io[3] orig URP, io[4] test URP, io[5] <- URP read back
; probe.c only calls this with TC.E clear in both values
t_tc4w:
        move.l  (a0),d0
        move.l  4(a0),d1
        dc.w    $4e7b,$1003             ; movec d1,tc
        dc.w    $4e7a,$2003             ; movec tc,d2
        dc.w    $4e7b,$0003             ; movec d0,tc
        move.l  d2,8(a0)
        move.l  12(a0),d0
        move.l  16(a0),d1
        dc.w    $4e7b,$1806             ; movec d1,urp
        dc.w    $4e7a,$2806             ; movec urp,d2
        dc.w    $4e7b,$0806             ; movec d0,urp
        move.l  d2,20(a0)
        rts

t_pmove_rd:
        dc.w    $f010,$4200             ; pmove tc,(a0)
        rts

t_pmove_tt:
        dc.w    $f010,$0a00             ; pmove tt0,(a0)
        lea     4(a0),a1
        dc.w    $f011,$0e00             ; pmove tt1,(a1)
        rts

; io[0] orig TC, io[1] test TC (E clear), io[2] <- read back
t_pmove_w:
        lea     4(a0),a1
        dc.w    $f011,$4000             ; pmove (a1),tc
        lea     8(a0),a1
        dc.w    $f011,$4200             ; pmove tc,(a1)
        dc.w    $f010,$4000             ; pmove (a0),tc
        rts

; 68030 MMU table walk test.  Enables translation for a few instructions with
; an identity map made of 16 early-termination page descriptors (256 MB each)
; and lets the MMU set the U bit of the descriptors it uses.
; io[0]    table address (16 longs, 16-byte aligned, prepared by probe.c)
; io[1,2]  CRP to load (DT=2, table address)
; io[3]    TC to load (E=1)
; io[4,5]  <- original CRP
; io[6]    original TC (E clear), loaded again afterwards
; io[7]    <- 1 when the original CRP was restored, 0 when it had DT=0
; io[8,9]  <- original TT0/TT1 (disabled during the test, then restored)
; io[10]   scratch (0, the disabled TT value)
t_mmu030u:
        move.l  #mmu030_cleanup,sv_cleanup
        ; Transparent translation (e.g. A3000 Kickstart sets TT0) would match
        ; the test access, so no table walk happens and an MMU looks absent:
        ; save TT0/TT1 and disable them for the test (restored in cleanup).
        dc.w    $f028,$0a00,$0020       ; pmove tt0,32(a0)
        dc.w    $f028,$0e00,$0024       ; pmove tt1,36(a0)
        clr.l   40(a0)
        dc.w    $f028,$0800,$0028       ; pmove 40(a0),tt0      disabled
        dc.w    $f028,$0c00,$0028       ; pmove 40(a0),tt1      disabled
        dc.w    $f028,$4e00,$0010       ; pmove crp,16(a0)
        dc.w    $f028,$4c00,$0004       ; pmove 4(a0),crp
        dc.w    $f028,$4000,$000c       ; pmove 12(a0),tc       translation on
        nop
        move.l  (a0),d0                 ; data access through the MMU
        nop
        dc.w    $f028,$4000,$0018       ; pmove 24(a0),tc       translation off
        rts                             ; sv_resume runs the cleanup

; also reached after a trap during t_mmu030u
mmu030_cleanup:
        dc.w    $f028,$4000,$0018       ; pmove 24(a0),tc  (orig, E clear)
        dc.w    $f028,$0800,$0020       ; pmove 32(a0),tt0 (orig)
        dc.w    $f028,$0c00,$0024       ; pmove 36(a0),tt1 (orig)
        clr.l   28(a0)
        move.l  16(a0),d0
        and.w   #3,d0
        beq.s   .nocrp                  ; DT=0 would be a configuration error
        dc.w    $f028,$4c00,$0010       ; pmove 16(a0),crp
        move.l  #1,28(a0)
.nocrp:
        dc.w    $4e7a,$0002             ; movec cacr,d0
        or.w    #$0800,d0               ; CD: clear the data cache
        dc.w    $4e7b,$0002             ; movec d0,cacr
        rts

t_fpu:
        moveq   #0,d0
        dc.w    $f200,$b000             ; fmove.l fpcr,d0
        move.l  d0,(a0)
        rts

; io[0..63]    <- FSAVE of the state on entry (restored with FRESTORE)
; io[64..127]  <- FSAVE after one FPU instruction (idle frame)
t_fsave:
        dc.w    $f310                   ; fsave (a0)
        dc.w    $f200,$b000             ; fmove.l fpcr,d0
        lea     256(a0),a1
        dc.w    $f311                   ; fsave (a1)
        dc.w    $f350                   ; frestore (a0)
        rts

;----------------------------------------------------------------------------
        section .bss,bss

sv_args:        ds.l    4               ; id, io, vbr, install
sv_trap:        ds.l    1
sv_spsave:      ds.l    1
sv_cleanup:     ds.l    1
_amt_probe_trappc:
                ds.l    1
vec_save:       ds.l    NVEC

        end
