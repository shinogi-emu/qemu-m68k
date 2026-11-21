/*
 * fast-fpu must not change which FP instructions trap.
 *
 * The 68040 and the 68060 leave different parts of the 68881/68882 to
 * software.  This model, in exact mode:
 *
 *  - computes the transcendentals, FGETEXP, FGETMAN, FMOD, FREM, FSCALE and
 *    FSINCOS on the 68040, and sends them to the F-line vector on the
 *    68060, for its software package (FPSP);
 *  - sends FMOVECR to the F-line vector on both;
 *  - takes the unimplemented data type exception (vector 55) for a packed
 *    decimal operand on both;
 *  - takes the 68060's unimplemented effective address exception (vector
 *    60) for an immediate .X or .P source, FMOVEM of several control
 *    registers from immediates and FMOVEM.X with a dynamic list; on the
 *    68040 only the first and the last are accepted, an immediate .P is
 *    unimplemented data and the FMOVEM is an illegal instruction (vector 4);
 *  - sends FScc, FDBcc and FTRAPcc to the F-line vector on the 68060, and
 *    every FP instruction there while PCR's DFP bit disables the FPU.
 *
 * The guest runs one instruction of each kind and prints a letter for the
 * exception it took: '.' none, 'F' F-line (11), 'U' unimplemented data type
 * (55), 'E' unimplemented effective address (60), '?' anything else, then
 * the vector in hex.  'x' marks a 68060-only case on the 68040.  The make
 * rule runs it with fast-fpu off and on and wants from both the line exact
 * mode gives on that CPU, the same as before fast-fpu existed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

        .equ    GF,             0xff008000

/*
 * One instruction, with fp0 = 2.0 and fp1 = 0.5 on entry; NOFP leaves the
 * FPU alone before it, for the disabled-FPU case.
 */
#define NOFP(...)                                                       \
        lea     9f,%a3;                                                 \
        move.l  %a3,resume;                                             \
        clr.l   vec;                                                    \
        __VA_ARGS__;                                                    \
9:      bsr     put

#define CASE(...)                                                       \
        fmove.x two,%fp0;                                               \
        fmove.x half,%fp1;                                              \
        NOFP(__VA_ARGS__)

        .text
        .globl  _start
_start:
        lea     stack_top,%sp
        lea     vectors,%a0
        move.l  %a0,%a1
        move.w  #255,%d0
1:      move.l  #trap,(%a1)+
        dbra    %d0,1b
        movec   %a0,%vbr

        /* A 68060 reads PCR; a 68040 takes the illegal instruction trap. */
        clr.l   is060
        move.l  #2f,resume
        clr.l   vec
        .word   0x4e7a,0x0808           /* movec %pcr,%d0 */
2:      tst.l   vec
        bne     3f
        move.l  #1,is060
3:
        lea     packed,%a0
        lea     scratch,%a2
        moveq   #3,%d1

        /* Monadic and dyadic instructions the 68060 leaves to its FPSP. */
        CASE(fsinh.x %fp1,%fp0)
        CASE(flognp1.x %fp1,%fp0)
        CASE(fetoxm1.x %fp1,%fp0)
        CASE(ftanh.x %fp1,%fp0)
        CASE(fatan.x %fp1,%fp0)
        CASE(fasin.x %fp1,%fp0)
        CASE(fatanh.x %fp1,%fp0)
        CASE(fsin.x %fp1,%fp0)
        CASE(ftan.x %fp1,%fp0)
        CASE(fetox.x %fp1,%fp0)
        CASE(ftwotox.x %fp1,%fp0)
        CASE(ftentox.x %fp1,%fp0)
        CASE(flogn.x %fp1,%fp0)
        CASE(flog10.x %fp1,%fp0)
        CASE(flog2.x %fp1,%fp0)
        CASE(fcosh.x %fp1,%fp0)
        CASE(facos.x %fp1,%fp0)
        CASE(fcos.x %fp1,%fp0)
        CASE(fgetexp.x %fp1,%fp0)
        CASE(fgetman.x %fp1,%fp0)
        CASE(fmod.x %fp1,%fp0)
        CASE(frem.x %fp1,%fp0)
        CASE(fscale.x %fp1,%fp0)
        CASE(fsincos.x %fp1,%fp2:%fp0)
        CASE(fmovecr.x #0,%fp0)

        /* What both implement. */
        CASE(fmove.x %fp1,%fp0)
        CASE(fint.x %fp1,%fp0)
        CASE(fintrz.x %fp1,%fp0)
        CASE(fsqrt.x %fp1,%fp0)
        CASE(fabs.x %fp1,%fp0)
        CASE(fneg.x %fp1,%fp0)
        CASE(fadd.x %fp1,%fp0)
        CASE(fsub.x %fp1,%fp0)
        CASE(fmul.x %fp1,%fp0)
        CASE(fdiv.x %fp1,%fp0)
        CASE(fsglmul.x %fp1,%fp0)
        CASE(fsgldiv.x %fp1,%fp0)
        CASE(fcmp.x %fp1,%fp0)
        CASE(ftst.x %fp1)
        CASE(fsadd.x %fp1,%fp0)
        CASE(fdmul.x %fp1,%fp0)
        CASE(fmove.l %d1,%fp0)
        CASE(fmove.s single,%fp0)
        CASE(fmove.d double,%fp0)
        CASE(fmove.l %fp0,%d2)
        CASE(fmove.d %fp0,(%a2))
        CASE(fbne    .+4)

        /* Packed decimal: unimplemented data type on both. */
        CASE(fmove.p (%a0),%fp0)
        CASE(fadd.p  (%a0),%fp0)
        CASE(.word 0xf212,0x6c00)     /* fmove.p %fp0,(%a2){#0} */

        /* Immediate .X and .P, FMOVEM: the 68060's unimplemented EA. */
        /* fmove.x #1.0,%fp0 */
        CASE(.word 0xf23c,0x4800,0x3fff,0,0x8000,0,0,0)
        /* fadd.x #1.0,%fp0 */
        CASE(.word 0xf23c,0x4822,0x3fff,0,0x8000,0,0,0)
        /* fmove.p #+1.0E+0,%fp0 */
        CASE(.word 0xf23c,0x4c00,0,1,0,0,0,0)
        /* fmovem.l #0,#0,%fpcr/%fpsr */
        CASE(.word 0xf23c,0x9800,0,0,0,0)
        moveq   #0,%d0
        /* fmovem.x %d0,(%a2), dynamic list (empty) */
        CASE(.word 0xf212,0xf800)

        /* Conditional instructions the 68060 leaves to its FPSP. */
        CASE(fsne    %d3)
        CASE(ftrapf)

        tst.l   is060
        bne     4f
        move.l  #'x',GF
        move.l  #'x',GF
        bra     5f
4:
        moveq   #0,%d0
        /* fdbf %d0,.+6 */
        CASE(.word 0xf248,0x0000,0x0002)
        /* PCR's DFP bit disables the FPU. */
        moveq   #2,%d0
        .word   0x4e7b,0x0808           /* movec %d0,%pcr */
        NOFP(fadd.x %fp1,%fp0)
        moveq   #0,%d0
        .word   0x4e7b,0x0808           /* movec %d0,%pcr */
5:
        move.l  #2,0xff009004
        stop    #0x2700

/* Prints the letter for vec. */
put:
        move.l  vec,%d0
        moveq   #'.',%d1
        tst.l   %d0
        beq     9f
        moveq   #'F',%d1
        cmpi.l  #11,%d0
        beq     9f
        moveq   #'U',%d1
        cmpi.l  #55,%d0
        beq     9f
        moveq   #'E',%d1
        cmpi.l  #60,%d0
        beq     9f
        move.l  #'?',GF
        move.l  %d0,%d1
        lsr.l   #4,%d1
        bsr     nib
        move.l  %d0,%d1
        bsr     nib
        rts
9:      move.l  %d1,GF
        rts

nib:    andi.l  #0xf,%d1
        cmpi.l  #9,%d1
        ble     1f
        addi.l  #'a'-10,%d1
        bra     2f
1:      addi.l  #'0',%d1
2:      move.l  %d1,GF
        rts

/* Records the vector, then resumes after the instruction, at resume. */
trap:
        move.w  6(%sp),%d0
        andi.l  #0x0fff,%d0
        lsr.l   #2,%d0
        move.l  %d0,vec
        lea     stack_top,%sp
        move.l  resume,-(%sp)
        rts

        .data
        .balign 4
two:    .long   0x40000000,0x80000000,0
half:   .long   0x3ffe0000,0x80000000,0
single: .long   0x3fc00000
double: .long   0x3ff80000,0
packed: .long   0x00000001,0,0              /* +1.0E+0 */

        .bss
        .balign 4
is060:      .long   0
vec:        .long   0
resume:     .long   0
scratch:    .space  16
vectors:    .space  1024
            .space  4096
stack_top:
