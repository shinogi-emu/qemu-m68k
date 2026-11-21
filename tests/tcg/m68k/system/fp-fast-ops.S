/*
 * fast-fpu: which helpers the translator emits.  The make rule runs this
 * guest under -d op with fast-fpu on and off and reads the op listing: with
 * fast-fpu on, the FP instructions are ffast_op, ffast_op_raw and
 * ffast_store calls, with no ftst after them, and FBcc/FScc read FPSR
 * without calling get_fpsr; with fast-fpu off, none of that.  It prints P
 * when it has run to the end.
 *
 * Assembled with M68060 defined for the 68060, which has no FScc: it would
 * take the F-line exception, for the 68060 software package.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

        .equ    GF,             0xff008000

        .text
        .globl  _start
_start:
        lea     stack_top,%sp
        fmove.x one,%fp0                /* ffast_op (.X via fp_result) */
        fadd.x  %fp0,%fp0               /* ffast_op */
        moveq   #3,%d1
        fmove.l %d1,%fp1                /* ffast_op_raw */
        fmul.s  three,%fp1              /* ffast_op_raw */
        fsqrt.x %fp1                    /* ffast_op */
        fmove.l %fp1,%d2                /* ffast_store */
        fcmp.x  %fp0,%fp1
        fbeq    fail                    /* IEEE-aware predicate */
        fbgt    1f                      /* nonaware predicate */
        bra     fail
1:
.ifndef M68060
        fsne    %d3                     /* FScc */
        tst.b   %d3
        beq     fail
.endif
        cmpi.l  #3,%d2
        bne     fail
        move.l  #'P',GF
        bra     exit
fail:
        move.l  #'F',GF
exit:
        move.l  #2,0xff009004
        stop    #0x2700

        .data
        .balign 4
one:    .long   0x3fff0000,0x80000000,0
three:  .long   0x40400000

        .bss
        .balign 4
        .space  4096
stack_top:
