/*
 * fast-fpu and TB chaining.
 *
 * Whether a TB is fast depends on FPCR's enable byte.  A TB that is not fast
 * must therefore not be chained (goto_tb) past an FPCR write: a link made
 * while the next TB was fast would later lead straight into that fast TB with
 * an exception enabled, and the instruction behind the link would run on the
 * fast path.
 *
 * The subroutine f_fpcr is called twice, entered with a TB that is not fast
 * both times (INEX2 enabled).  In the first pass it clears FPCR, so its
 * branch is linked to a fast TB; in the second it enables INEX2 again, and
 * the FSIN behind the branch must give what exact mode gives.
 *
 * This model delivers no FP exceptions, and the fast path gives exact mode's
 * results bit for bit for everything but the transcendentals.  So the test
 * tells the paths apart by an FSIN operand whose result differs between the
 * host library and exact mode, and checks first that it does: the reference
 * results come from an FSIN in a TB that starts with INEX2 enabled (exact),
 * and from one in a TB that starts with FPCR clear (fast, if fast-fpu is on).
 *
 * Prints P when the second pass gave the exact result and the two references
 * differ, p when they are equal (fast-fpu off: there is nothing to tell
 * apart), F when the second pass took the fast path, or s on a 68060, whose
 * FSIN is an F-line trap for its software package, so that no instruction
 * the 68060 computes differs between the two paths.  ! is an unexpected
 * exception.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

        .equ    GF,             0xff008000
        .equ    FPCR_INEX2,     0x0200

        .text
        .globl  _start
_start:
        lea     stack_top,%sp
        lea     vectors,%a0
        move.l  %a0,%a1
        move.w  #255,%d0
1:      move.l  #unexpected,(%a1)+
        dbra    %d0,1b
        move.l  #line_f,11*4(%a0)
        movec   %a0,%vbr

        /* A 68060 takes the F-line trap for FSIN: nothing to test there. */
        clr.l   fline
        fmove.x arg,%fp1
        fsin.x  %fp1,%fp0
        tst.l   fline
        bne     skipped

        /* The exact reference: a TB that starts with INEX2 enabled. */
        move.l  #FPCR_INEX2,%d1
        fmove.l %d1,%fpcr
        fsin.x  %fp1,%fp2
        fmove.x %fp2,ref_exact
        /* The fast reference: a TB that starts with FPCR clear. */
        fmove.l #0,%fpcr
        fsin.x  %fp1,%fp3
        fmove.x %fp3,ref_fast

        move.l  #FPCR_INEX2,%d1
        fmove.l %d1,%fpcr               /* f_fpcr entered not fast */
        moveq   #0,%d0                  /* ... and leaves FPCR clear */
        bsr     f_fpcr
        move.l  #FPCR_INEX2,%d1
        fmove.l %d1,%fpcr
        move.l  #FPCR_INEX2,%d0         /* now it leaves INEX2 enabled */
        bsr     f_fpcr
        fmove.x %fp0,res
        fmove.l #0,%fpcr

        lea     res,%a0
        lea     ref_exact,%a1
        bsr     cmp12
        bne     fail
        lea     ref_fast,%a0
        lea     ref_exact,%a1
        bsr     cmp12
        beq     insensitive
        move.l  #'P',GF
        bra     exit
insensitive:
        move.l  #'p',GF
        bra     exit
fail:
        move.l  #'F',GF
        bra     exit
skipped:
        move.l  #'s',GF
exit:
        move.l  #2,0xff009004
        stop    #0x2700

/* The bra.w ends the first TB with a chainable jump. */
f_fpcr:
        fmove.l %d0,%fpcr
        bra.w   1f
1:      fsin.x  %fp1,%fp0
        rts

/* Z set when the 12 bytes at a0 and a1 are equal. */
cmp12:
        cmpm.l  (%a0)+,(%a1)+
        bne     1f
        cmpm.l  (%a0)+,(%a1)+
        bne     1f
        cmpm.l  (%a0)+,(%a1)+
1:      rts

/* 68060 FPU unimplemented instruction: the stacked PC is the next one. */
line_f:
        move.l  #1,fline
        rte

unexpected:
        move.l  #'!',GF
        bra     exit

        .data
        .balign 4
/* 10000.0: the library and exact mode disagree on its sine. */
arg:        .long   0x400c0000,0x9c400000,0

        .bss
        .balign 4
fline:      .long   0
res:        .space  12
ref_exact:  .space  12
ref_fast:   .space  12
vectors:    .space  1024
            .space  4096
stack_top:
