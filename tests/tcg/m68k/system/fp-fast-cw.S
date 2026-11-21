/*
 * fast-fpu: the x87 control word after a guest exception.
 *
 * A guest exception leaves generated code through cpu_loop_exit(), a longjmp
 * back into cpu_exec().  On Windows that longjmp restores the x87 control
 * word saved by setjmp(), the host thread's own (0x027f, 53-bit precision),
 * so the fast path must not take the word it loaded last to be still in
 * force.  Each operation below runs, the guest takes an exception, and the
 * same operation runs again, wanting the same control word as the first
 * time.  One letter per operation, from the second run:
 *
 *   E  FADD.X 1.0 + 2^-60 is exact at extended precision (D: 53-bit result)
 *   S  FSADD.X 1.0 + 2^-30 rounds to single precision, 1.0 (X: it did not)
 *   T  FINTRZ.X 2.75 truncates to 2.0 (R: rounded to nearest, 3.0)
 *   ?  anything else; !  an unexpected exception
 *
 * The three run with no exception in between first, then around a line-A
 * trap, a divide by zero and a privilege violation: ESTESTESTEST.  Run with
 * fast-fpu off and on.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

        .equ    GF,             0xff008000

        .text
        .globl  _start
_start:
        lea     stack_top,%sp
        lea     ustack_top,%a0
        move.l  %a0,%usp
        lea     vectors,%a0
        move.l  %a0,%a1
        move.w  #255,%d0
1:      move.l  #unexpected,(%a1)+
        dbra    %d0,1b
        move.l  #zero_divide,5*4(%a0)
        move.l  #privilege,8*4(%a0)
        move.l  #line_a,10*4(%a0)
        movec   %a0,%vbr

        lea     no_exception,%a0
        bsr     round
        lea     raise_line_a,%a0
        bsr     round
        lea     raise_zero_divide,%a0
        bsr     round
        lea     raise_privilege,%a0
        bsr     round
exit:
        move.l  #2,0xff009004
        stop    #0x2700

/* Each operation, the exception (a0), the operation again, its letter. */
round:
        bsr     op_add
        jsr     (%a0)
        bsr     op_add
        bsr     put_add
        bsr     op_sadd
        jsr     (%a0)
        bsr     op_sadd
        bsr     put_sadd
        bsr     op_intrz
        jsr     (%a0)
        bsr     op_intrz
        bsr     put_intrz
        rts

op_add:
        fmove.x one,%fp0
        fadd.x  two_m60,%fp0
        fmove.x %fp0,res
        rts

op_sadd:
        fmove.x one,%fp0
        fsadd.x two_m30,%fp0
        fmove.x %fp0,res
        rts

op_intrz:
        fintrz.x two_75,%fp0
        fmove.x %fp0,res
        rts

/* The results, checked with integer instructions only. */
put_add:
        moveq   #'?',%d0
        cmpi.l  #0x3fff0000,res
        bne     put
        cmpi.l  #0x80000000,res+4
        bne     put
        move.l  res+8,%d1
        moveq   #'E',%d0
        cmpi.l  #0x00000008,%d1
        beq     put
        moveq   #'D',%d0
        tst.l   %d1
        beq     put
        moveq   #'?',%d0
        bra     put

put_sadd:
        moveq   #'?',%d0
        cmpi.l  #0x3fff0000,res
        bne     put
        tst.l   res+8
        bne     put
        move.l  res+4,%d1
        moveq   #'S',%d0
        cmpi.l  #0x80000000,%d1
        beq     put
        moveq   #'X',%d0
        cmpi.l  #0x80000002,%d1
        beq     put
        moveq   #'?',%d0
        bra     put

put_intrz:
        moveq   #'?',%d0
        cmpi.l  #0x40000000,res
        bne     put
        tst.l   res+8
        bne     put
        move.l  res+4,%d1
        moveq   #'T',%d0
        cmpi.l  #0x80000000,%d1
        beq     put
        moveq   #'R',%d0
        cmpi.l  #0xc0000000,%d1
        beq     put
        moveq   #'?',%d0
put:
        move.l  %d0,GF
        rts

/* The exceptions, each through cpu_loop_exit(). */
no_exception:
        rts

raise_line_a:
        .word   0xa000
        rts

raise_zero_divide:
        moveq   #1,%d0
        moveq   #0,%d1
        divu.w  %d1,%d0
        rts

raise_privilege:
        andi.w  #0xdfff,%sr             /* to user mode */
        move.w  #0x2700,%sr             /* privileged: 4 bytes */
        rts                             /* back in supervisor mode */

/* The handlers. */
line_a:                                 /* stacked PC: the line-A word */
        addq.l  #2,2(%sp)
        rte

zero_divide:                            /* stacked PC: the next insn */
        rte

privilege:                              /* stacked PC: the insn; S again */
        ori.w   #0x2000,(%sp)
        addq.l  #4,2(%sp)
        rte

unexpected:
        move.l  #'!',GF
        bra     exit

        .data
        .balign 4
one:     .long  0x3fff0000,0x80000000,0
two_m60: .long  0x3fc30000,0x80000000,0
two_m30: .long  0x3fe10000,0x80000000,0
two_75:  .long  0x40000000,0xb0000000,0

        .bss
        .balign 4
res:        .space  12
vectors:    .space  1024
            .space  4096
ustack_top:
            .space  4096
stack_top:
