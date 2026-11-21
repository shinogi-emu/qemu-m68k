/*
 * FPU timing guest: 20,000,000 iterations of a register loop over the
 * arithmetic that fast-fpu is meant to speed up.  The operands stay normal
 * throughout (fp0 drifts up by about 1e-7 per iteration, plus 0.25 from fp3).
 * Run it with fast-fpu off and on and compare wall-clock time; the result is
 * not checked here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

        .text
        .globl  _start
_start:
        lea     0x00100000,%sp
        fmove.l #0,%fpcr
        lea     quarter,%a0
        lea     scratch,%a1
        fmove.d one,%fp0
        fmove.d one,%fp1
        fmove.d onepluseps,%fp2
        move.l  #20000000,%d0
1:      fadd.x  %fp1,%fp0
        fmul.x  %fp2,%fp0
        fsub.x  %fp1,%fp0
        fdiv.x  %fp2,%fp0
        fmove.d (%a0),%fp3
        fadd.x  %fp3,%fp0
        fmove.d %fp0,(%a1)
        subq.l  #1,%d0
        bne     1b
        move.l  #2,0xff009004
        stop    #0x2700

        .data
        .balign 4
one:        .long   0x3ff00000, 0
onepluseps: .long   0x3ff00000, 0x1ad7f29b   /* 1.0000001 */
quarter:    .long   0x3fd00000, 0
        .bss
        .balign 4
scratch:    .space  8
