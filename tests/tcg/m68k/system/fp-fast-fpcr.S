/*
 * fast-fpu: the run-time check after an FPCR write from a register or memory.
 *
 * In a fast TB, an FPCR write whose value is not known at translate time is
 * followed by a check of the new enable byte: when it is non-zero, the TB is
 * left for a lookup, so the next instruction runs on the exact path; when it
 * is zero, the TB carries on on the fast path.  Each case below writes FPCR
 * inside a fast TB and runs FSIN in the same TB:
 *
 *   1. FMOVEM.L (a0),FPCR/FPSR/FPIAR enabling INEX2: the exact result
 *   2. the same with the enable byte zero:             the fast result
 *   3. FMOVE.L Dn,FPCR enabling INEX2:                 the exact result
 *   4. the same with the enable byte zero:             the fast result
 *
 * This model delivers no FP exceptions, and fast mode differs from exact
 * mode only in the transcendentals, so the paths are told apart by an FSIN
 * operand whose result differs between them (fp-fast-chain's).  The
 * references come from an FSIN in a TB entered by a branch with INEX2
 * enabled (exact) and with FPCR clear (fast, if fast-fpu is on).  The FMOVEM
 * image holds the same value in all three longs, so the order the registers
 * are loaded in does not matter.
 *
 * One letter per case: E the exact result, X the fast result, p the
 * references are equal (fast-fpu off: nothing to tell apart), ? anything
 * else.  With fast-fpu on: EXEX; off: pppp; s on a 68060, whose FSIN is an
 * F-line trap; ! an unexpected exception.
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

        /*
         * The references: sin_ref is entered by a branch, which looks up
         * its TB under the FPCR then in force, so they do not depend on the
         * check under test.  Exact with INEX2 enabled, fast with FPCR clear.
         */
        move.l  #FPCR_INEX2,%d1
        fmove.l %d1,%fpcr
        bsr     sin_ref
        fmove.x %fp2,ref_exact
        fmove.l #0,%fpcr
        bsr     sin_ref
        fmove.x %fp2,ref_fast

        /*
         * Each case starts after an immediate write of a clear FPCR, which
         * a fast TB carries on past and any other TB ends with a lookup:
         * the write under test runs in a fast TB, with the FSIN behind it.
         */
        fmove.l #0,%fpcr
        lea     image_inex2,%a0
        fmovem.l (%a0),%fpcr/%fpsr/%fpiar
        fsin.x  %fp1,%fp0
        bsr     put

        fmove.l #0,%fpcr
        lea     image_zero,%a0
        fmovem.l (%a0),%fpcr/%fpsr/%fpiar
        fsin.x  %fp1,%fp0
        bsr     put

        fmove.l #0,%fpcr
        move.l  #FPCR_INEX2,%d1
        fmove.l %d1,%fpcr
        fsin.x  %fp1,%fp0
        bsr     put

        fmove.l #0,%fpcr
        moveq   #0,%d1
        fmove.l %d1,%fpcr
        fsin.x  %fp1,%fp0
        bsr     put

        fmove.l #0,%fpcr
        bra     exit
skipped:
        move.l  #'s',GF
exit:
        move.l  #2,0xff009004
        stop    #0x2700

sin_ref:
        fsin.x  %fp1,%fp2
        rts

/* The letter for the result in fp0. */
put:
        fmove.x %fp0,res
        lea     ref_fast,%a0
        lea     ref_exact,%a1
        bsr     cmp12
        bne     1f
        lea     res,%a0
        lea     ref_exact,%a1
        bsr     cmp12
        beq     2f
        moveq   #'?',%d0
        bra     9f
2:      moveq   #'p',%d0
        bra     9f
1:      lea     res,%a0
        lea     ref_exact,%a1
        bsr     cmp12
        bne     3f
        moveq   #'E',%d0
        bra     9f
3:      lea     res,%a0
        lea     ref_fast,%a1
        bsr     cmp12
        bne     4f
        moveq   #'X',%d0
        bra     9f
4:      moveq   #'?',%d0
9:      move.l  %d0,GF
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
/* -2.1461538...: exact mode's sine is one ulp from the libraries' (see
 * fp-fast-chain.S). */
arg:         .long   0xc0000000,0x895a95a9,0x5a95a95b
image_inex2: .long   FPCR_INEX2,FPCR_INEX2,FPCR_INEX2
image_zero:  .long   0,0,0

        .bss
        .balign 4
fline:      .long   0
res:        .space  12
ref_exact:  .space  12
ref_fast:   .space  12
vectors:    .space  1024
            .space  4096
stack_top:
