/*
 * m68k FPU: fast-fpu helpers on the host x87
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/*
 * In fast-fpu mode (TB_FLAGS_FAST_FPU) each FP data instruction is one call
 * to a helper below.  The helper computes on the host x87 when every operand
 * it reads is zero or a finite normal number, and accepts the x87 result only
 * when that is zero or normal too and the x87 raised nothing but inexact.  It
 * then writes what exact mode writes for such a result.  Anything else, and
 * every instruction the x87 path does not cover, runs the exact softfloat
 * code of that instruction inside the same call.  An attempt writes nothing
 * to the guest before its result is accepted.
 *
 * The x87 exception flags are sticky, and clearing them (fnclex) costs more
 * than the rest of a fast instruction put together.  So they are cleared only
 * after the x87 raised something other than inexact, which sends the
 * instruction to the exact path anyway; those flags are clear on entry.  The
 * inexact flag may be left over from earlier instructions, so whether this
 * one was inexact is worked out from its operands (ffast_inexact()).
 *
 * FPSR is kept as exact mode keeps it: the condition codes (and the FMOD/FREM
 * quotient) in env->fpsr, the accrued exceptions as sticky softfloat flags
 * in fp_status, which cpu_m68k_get_fpsr() merges in when FPSR is read.  An
 * instruction the x87 completes sets the condition codes and, when it was
 * inexact, the softfloat inexact flag; no other exception is possible for
 * the results it accepts.  The exception status byte is not touched by any
 * instruction, in either mode.
 *
 * The TB flag is set only on x86_64 hosts, with FPCR's exception-enable byte
 * zero.  Neither path can raise a guest exception, so the helpers can be
 * TCG_CALL_NO_RWG.
 *
 * On other hosts this file compiles to nothing.
 */

#include "qemu/osdep.h"

#if defined(__x86_64__)

#include <math.h>
#include "qemu/host-utils.h"
#include "host/cpuinfo.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat.h"

/* x87 status word exception flags other than inexact */
#define X87_IE          0x0001
#define X87_DE          0x0002
#define X87_ZE          0x0004
#define X87_OE          0x0008
#define X87_UE          0x0010
#define X87_NOT_INEXACT (X87_IE | X87_DE | X87_ZE | X87_OE | X87_UE)

/* x87 control word fields, as fast_fpu_control_word() builds them */
#define X87_PC_MASK     0x0300
#define X87_PC_S        0x0000
#define X87_PC_D        0x0200
#define X87_RC_MASK     0x0c00
#define X87_RC_DOWN     0x0400
#define X87_RC_UP       0x0800
#define X87_RC_CHOP     0x0c00

/*
 * RN, 64-bit precision, all exceptions masked: what libm's code assumes, and
 * FPCR's own word for extended precision rounding to nearest.
 */
#define X87_CW_DEFAULT  0x037f

#define FFAST_BIAS      0x3fff
#define FFAST_SGL_MASK  0xffffff0000000000ull   /* a significand's top 24 */

/*
 * The x87 control word.  Each site that needs a particular word reads the
 * one the x87 holds (fnstcw) and loads its own (fldcw) only when they differ.
 * No copy is kept in software, as one would go stale behind the code's back:
 * Microsoft's x64 longjmp() restores the control word that setjmp() saved, so
 * every cpu_loop_exit() inside cpu_exec() puts the vCPU thread's own word
 * back (0x027f on Win64), and linux-user's "current TB modified" path
 * siglongjmps out of a signal handler, under the x87's default state.
 * fast_fpu_control_word() builds every word from 0x007f, with the reserved
 * bit 6 set as fnstcw reads it back, so the two compare directly.
 *
 * Every class of control word has its own fldcw, named by site: FPCR's word,
 * the FS and FD forms, FINTRZ, FSGLDIV, the stores, the raw conversion and
 * the library calls.  On a Zen 5 an fldcw that loads a different value than
 * it did last time costs about 40 ns, while one that keeps loading the same
 * value costs next to nothing, however often the word in force changes
 * between sites.  The site name is part of the asm text, so the compiler
 * cannot merge the sites.
 */
static inline uint16_t ffast_cw_now(void)
{
    uint16_t cw;

    asm volatile("fnstcw %0" : "=m"(cw));
    return cw;
}

#define FFAST_LOAD_CW(cw, site)                                         \
    do {                                                                \
        uint16_t cw_ = (cw);                                            \
                                                                        \
        if (cw_ != ffast_cw_now()) {                                    \
            asm volatile("fldcw %0\t# " site : : "m"(cw_));             \
        }                                                               \
    } while (0)

/*
 * The same for the stores: fist(p) and fst(p) round to the destination
 * format and ignore the precision field, so every other field has to be
 * FPCR's.
 */
static inline void ffast_load_store_cw(uint16_t cw)
{
    if ((ffast_cw_now() ^ cw) & ~X87_PC_MASK) {
        asm volatile("fldcw %0\t# store" : : "m"(cw));
    }
}

static inline void ffast_clear_flags(void)
{
    asm volatile("fnclex");
}

/*
 * TCGCPUOps cpu_exec_enter and cpu_exec_exit, for every m68k CPU.  Generated
 * code and the fast helpers run under FPCR's control words; host code outside
 * cpu_exec() gets its own word back (callee-saved in the ABIs; Win64 starts
 * threads with 0x027f, Linux with 0x037f).  Nothing is loaded when the x87
 * holds the thread's word already.
 *
 * Inside cpu_exec(), generated code may call helpers, device emulation and,
 * on Windows, Win32 APIs with one of FPCR's words loaded, where the Win64 ABI
 * expects calls to be made under the standard word.  Only host code computing
 * in long double could tell the difference; none was found on those paths.
 */
void m68k_cpu_exec_enter(CPUState *cs)
{
    M68K_CPU(cs)->fast_fpu_host_cw = ffast_cw_now();
}

void m68k_cpu_exec_exit(CPUState *cs)
{
    uint16_t cw = M68K_CPU(cs)->fast_fpu_host_cw;

    if (ffast_cw_now() != cw) {
        asm volatile("fldcw %0" : : "m"(cw));
    }
}

/*
 * One block per x87 operation: load the operands from memory, compute, store
 * the result, read the status word.  Every block leaves the x87 stack empty.
 * FFAST_DYADIC computes dest OP src, with st(0) = dest and st(1) = src.
 * X87_ROUND rounds st(0) to the precision and direction of the control word
 * (multiplying by 1.0 keeps the sign of zero).
 */
#define FFAST_MONADIC(insns, res, src, sw)                              \
    asm volatile("fldt %[s]\n\t" insns "\n\t"                           \
                 "fstpt %[r]\n\t" "fnstsw %[w]"                         \
                 : [r] "=m"(*(res)), [w] "=a"(sw)                       \
                 : [s] "m"(*(src))                                      \
                 : "st", "st(1)")

#define FFAST_DYADIC(insn, res, dest, src, sw)                          \
    asm volatile("fldt %[s]\n\t" "fldt %[d]\n\t"                        \
                 insn " %%st(1), %%st\n\t" "fstp %%st(1)\n\t"           \
                 "fstpt %[r]\n\t" "fnstsw %[w]"                         \
                 : [r] "=m"(*(res)), [w] "=a"(sw)                       \
                 : [d] "m"(*(dest)), [s] "m"(*(src))                    \
                 : "st", "st(1)")

#define X87_ROUND       "fld1\n\t" "fmulp"

/* What the x87 path does for an opmode.  The dyadic kinds come last. */
typedef enum {
    FFAST_EXACT,        /* nothing: always the exact path */
    FFAST_MOVE,         /* FMOVE: a copy, as gen_fp_move() */
    FFAST_TST,
    FFAST_CMP,
    FFAST_ROUND,        /* FSMOVE, FDMOVE */
    FFAST_INT,          /* FINT, FINTRZ */
    FFAST_SQRT,
    FFAST_ABS,
    FFAST_NEG,
    FFAST_LIBM,         /* the transcendentals of one result */
    FFAST_SINCOS,
    FFAST_ADD,
    FFAST_SUB,
    FFAST_MUL,
    FFAST_DIV,
    FFAST_SGLMUL,
    FFAST_SGLDIV,
} FFastKind;

static FFastKind ffast_kind(uint32_t opmode)
{
    switch (opmode) {
    case 0x00:
        return FFAST_MOVE;
    case 0x3a:
        return FFAST_TST;
    case 0x38:
        return FFAST_CMP;
    case 0x40: case 0x44:
        return FFAST_ROUND;
    case 0x01: case 0x03:
        return FFAST_INT;
    case 0x04: case 0x41: case 0x45:
        return FFAST_SQRT;
    case 0x18: case 0x58: case 0x5c:
        return FFAST_ABS;
    case 0x1a: case 0x5a: case 0x5e:
        return FFAST_NEG;
    case 0x02: case 0x06: case 0x08: case 0x09: case 0x0a: case 0x0c:
    case 0x0d: case 0x0e: case 0x0f: case 0x10: case 0x11:
    case 0x14: case 0x15: case 0x16: case 0x19: case 0x1c: case 0x1d:
        return FFAST_LIBM;
    case 0x12:
#ifdef _WIN32
        return FFAST_EXACT;     /* FTENTOX, see the transcendentals */
#else
        return FFAST_LIBM;
#endif
    case 0x30 ... 0x37:
        return FFAST_SINCOS;
    case 0x22: case 0x62: case 0x66:
        return FFAST_ADD;
    case 0x28: case 0x68: case 0x6c:
        return FFAST_SUB;
    case 0x23: case 0x63: case 0x67:
        return FFAST_MUL;
    case 0x20: case 0x60: case 0x64:
        return FFAST_DIV;
    case 0x27:
        return FFAST_SGLMUL;
    case 0x24:
        return FFAST_SGLDIV;
    default:
        return FFAST_EXACT;
    }
}

/* The control word an opmode computes under. */
static uint16_t ffast_op_cw(CPUM68KState *env, uint32_t opmode)
{
    uint16_t cw = env->fast_fpu_cw;

    if (opmode & 0x40) {
        /* FSxxx and FDxxx: PREC_BEGIN(floatx80_precision_s or _d) */
        cw = (cw & ~X87_PC_MASK) | (opmode & 4 ? X87_PC_D : X87_PC_S);
    } else if (opmode == 0x03) {
        /* FINTRZ: float_round_to_zero, as helper_fitrunc() */
        cw |= X87_RC_CHOP;
    }
    return cw;
}

/* Significand bits a control word rounds to. */
static inline int ffast_prec(uint16_t cw)
{
    switch (cw & X87_PC_MASK) {
    case X87_PC_S:
        return 24;
    case X87_PC_D:
        return 53;
    default:
        return 64;
    }
}

/* Zero, or normal: exponent 1..0x7ffe with the explicit integer bit set. */
static inline bool ffast_operand_ok(const FPReg *f)
{
    uint16_t exp = f->l.upper & 0x7fff;

    if (exp == 0) {
        return f->l.lower == 0;
    }
    return exp != 0x7fff && (f->l.lower >> 63);
}

/* False for a .S or .D operand that is an infinity or a NaN. */
static inline bool ffast_raw_ok(uint64_t raw, int opsize)
{
    switch (opsize) {
    case OS_SINGLE:
        return (~raw & 0x7f800000) != 0;
    case OS_DOUBLE:
        return (~raw & 0x7ff0000000000000ull) != 0;
    default:
        return true;
    }
}

/*
 * Whether kind k can give an exact zero from these operands; any other zero
 * result is an underflow.
 */
static inline bool ffast_zero_ok(FFastKind k, const FPReg *dest,
                                 const FPReg *src)
{
    switch (k) {
    case FFAST_ADD:
    case FFAST_SUB:
    case FFAST_INT:
        return true;
    case FFAST_MUL:
    case FFAST_SGLMUL:
        return !dest->l.lower || !src->l.lower;
    case FFAST_DIV:
    case FFAST_SGLDIV:
        return !dest->l.lower;
    default:
        return !src->l.lower;
    }
}

/*
 * An x87 result that exact mode produces too: normal and below the top
 * binade (an overflow rounded towards zero stops at the largest finite
 * number), or a zero the operation gives exactly.  The class is checked as
 * well as the flags because a masked x87 underflow is flagged only when the
 * result is also inexact.
 */
static inline bool ffast_result_ok(const FPReg *r, bool zero_ok)
{
    uint16_t exp = r->l.upper & 0x7fff;

    if (exp == 0) {
        return zero_ok && r->l.lower == 0;
    }
    return exp < 0x7ffe && (r->l.lower >> 63);
}

/*
 * Under single or double precision, exact mode rounds the significand and
 * keeps the extended exponent range, as the x87 does.  A result outside the
 * range of that format is still left to the exact path, as on trees whose
 * softfloat has m68k range control (overflow or underflow reported on the
 * unrounded exponent): such results are rare, and one accepted set serves
 * both.  The exponent of the rounded result may be one more than the
 * unrounded one, so the x87 path takes zero, or a result r computed under
 * the control word cw whose exponent is strictly inside the range.
 */
static inline bool ffast_range_ok(const FPReg *r, uint16_t cw)
{
    int exp = r->l.upper & 0x7fff;

    if (exp == 0) {
        return true;
    }
    exp -= FFAST_BIAS;
    switch (cw & X87_PC_MASK) {
    case X87_PC_S:
        return exp > -126 && exp <= 127;
    case X87_PC_D:
        return exp > -1022 && exp <= 1023;
    default:
        return true;
    }
}

/*
 * A .S or .D store, whose exponent field is exp (all ones being expmax),
 * that exact mode makes too: zero exactly when the source is zero, otherwise
 * normal and below the top binade.  A rounded value in the lowest normal
 * binade is refused, as in ffast_try().
 */
static inline bool ffast_out_ok(uint32_t exp, uint32_t expmax, bool zero,
                                bool inexact)
{
    if (exp == 0) {
        return zero;
    }
    return exp < expmax - 1 && (exp != 1 || !inexact);
}

/*
 * Inexact, from the operands.
 *
 * Every operand the x87 path takes is zero or m * 2^(e - 63), with m the
 * 64-bit significand, top bit set, and e the unbiased exponent.  Every result
 * it accepts is zero or normal, neither denormalized nor overflowed, so the
 * x87 rounded once, at the p-th significant bit of the exact result (p = 24,
 * 53 or 64, from the control word's precision field).  So the result is
 * inexact exactly when the exact result has a bit set below that position,
 * which is what each test computes:
 *
 * - FSMOVE, FDMOVE, FABS, FNEG and the .S/.D stores: the exact result is the
 *   operand, sign aside, so a bit of m below its top p (ffast_lost()).
 * - Conversions: fild and fld are exact (an int32 has at most 32 significant
 *   bits, a .S or .D at most 53); rounding to FPCR's precision afterwards is
 *   the FSMOVE case.
 * - FINT, FINTRZ, and the .B/.W/.L stores (p = 64): round to an integer,
 *   then to p bits (ffast_int_inexact()).  If e < 0, the operand is a nonzero
 *   value below 1, which the first step changes.  If 0 <= e < p - 1, the
 *   integer has at most e + 2 <= p bits (e + 1, plus a carry), so the second
 *   step is exact and the first loses the bits below the binary point: the
 *   low 63 - e bits of m.  Otherwise the binary point is at or below the
 *   p-th bit, so every fraction bit is below it too.  A nonzero fraction
 *   makes the first step inexact; a zero fraction leaves m to the second.
 *   Either way a bit of m below its top p decides it.
 * - FMUL: the exact product of the significands, < 2^128, is computed in
 *   full; inexact when it has a bit below its top p (ffast_lost128()).
 * - FADD, FSUB (ffast_addsub_inexact()): with |a| >= |b| and d = ea - eb,
 *   (ma << d) +- mb is the exact result scaled by 2^(63 - eb), and fits in
 *   128 bits for d <= 64; inexact when it has a bit below its top p.  For
 *   d >= 65, 0 < |b| < 2^(ea - 64).  The result is more than |a| / 2, so its
 *   last bit is worth at least 2^(ea - 64), and a is a multiple of that, so
 *   a +- b falls between two results: always inexact.
 * - FDIV, FSQRT: the rounded result r is exact iff r * src == dest, or
 *   r * r == src.  On the significands that is rm * sm == dm << k with
 *   k = de - re - se + 63, or rm * rm == sm << k with k = se - 2 re + 63.
 *   The quotient of two significands is in (1/2, 2) and a root's exponent is
 *   half the operand's, so a normal result has k = 62, 63 or 64 and both
 *   sides fit in 128 bits.  k = 62 means the rounding carried into the next
 *   binade, which is inexact, and the comparison says so.
 * - FSGLMUL, FSGLDIV: each operand truncated to 24 bits first (inexact if
 *   either loses a bit), then FMUL or FDIV with p = 24 on what remains.
 */

/* Whether a significand has bits set below its top p. */
static inline bool ffast_lost(uint64_t m, int p)
{
    return p < 64 && (m << p) != 0;
}

/* The same for a nonzero 128-bit integer. */
static inline bool ffast_lost128(unsigned __int128 s, int p)
{
    uint64_t hi = s >> 64;
    int below = (hi ? 128 - clz64(hi) : 64 - clz64((uint64_t)s)) - p;

    return below > 0 && (s << (128 - below)) != 0;
}

/* Whether rounding f to an integer, then to p bits, changes it. */
static bool ffast_int_inexact(const FPReg *f, int p)
{
    int e = (f->l.upper & 0x7fff) - FFAST_BIAS;
    uint64_t m = f->l.lower;

    if (m == 0) {
        return false;
    }
    if (e < 0) {
        return true;
    }
    if (e < p - 1) {
        /* the integer fits in p bits: only the fraction is lost */
        return (m << (e + 1)) != 0;
    }
    /* the fraction lies below the top p bits */
    return ffast_lost(m, p);
}

/* Whether a + b, or a - b, has more than p significant bits. */
static bool ffast_addsub_inexact(const FPReg *a, const FPReg *b, bool sub,
                                 int p)
{
    uint64_t am = a->l.lower, bm = b->l.lower, tm;
    int ae = a->l.upper & 0x7fff, be = b->l.upper & 0x7fff, te;
    bool neg = ((a->l.upper ^ b->l.upper) & 0x8000) != 0;
    unsigned __int128 s;

    if (!am) {
        return ffast_lost(bm, p);
    }
    if (!bm) {
        return ffast_lost(am, p);
    }
    if (ae < be || (ae == be && am < bm)) {
        tm = am, am = bm, bm = tm;
        te = ae, ae = be, be = te;
    }
    /*
     * With |a| >= |b|, the result's last bit is worth at least 2^-64 of a's
     * leading bit; b is less than that when its exponent is 65 or more
     * below, so it cannot be represented in the sum.
     */
    if (ae - be > 64) {
        return true;
    }
    s = (unsigned __int128)am << (ae - be);
    s = neg != sub ? s - bm : s + bm;
    return s != 0 && ffast_lost128(s, p);
}

/*
 * Whether the exact result of kind k, of dest and src, differs from the x87
 * result r, which is normal or zero and rounded to p bits (see above).  The
 * exponents here are biased, hence the FFAST_BIAS in the shifts.
 */
static bool ffast_inexact(FFastKind k, const FPReg *r, const FPReg *dest,
                          const FPReg *src, int p)
{
    uint64_t rm = r->l.lower, dm = dest->l.lower, sm = src->l.lower;
    int re = r->l.upper & 0x7fff;
    int de = dest->l.upper & 0x7fff;
    int se = src->l.upper & 0x7fff;

    switch (k) {
    case FFAST_ROUND:
    case FFAST_ABS:
    case FFAST_NEG:
        return ffast_lost(sm, p);
    case FFAST_INT:
        return ffast_int_inexact(src, p);
    case FFAST_SQRT:
        /* exact when r * r == src */
        return sm && (unsigned __int128)rm * rm !=
                     (unsigned __int128)sm << (se - 2 * re + FFAST_BIAS + 63);
    case FFAST_MUL:
        return dm && sm && ffast_lost128((unsigned __int128)dm * sm, p);
    case FFAST_DIV:
        /* exact when r * src == dest */
        return dm && (unsigned __int128)rm * sm !=
                     (unsigned __int128)dm << (de - re - se + FFAST_BIAS + 63);
    case FFAST_ADD:
    case FFAST_SUB:
        return ffast_addsub_inexact(dest, src, k == FFAST_SUB, p);
    default:
        g_assert_not_reached();
    }
}

/* FTST's condition codes for zero or a normal number. */
static inline uint32_t ffast_cc(const FPReg *f)
{
    uint32_t cc = f->l.upper & 0x8000 ? FPSR_CC_N : 0;

    if (!(f->l.upper & 0x7fff)) {
        cc |= FPSR_CC_Z;
    }
    return cc;
}

/*
 * FCMP's condition codes (float_comp_to_cc) for dest compared with src, both
 * zero or normal: Z when equal, +0 and -0 included; N when less; none when
 * greater.
 */
static uint32_t ffast_compare(const FPReg *dest, const FPReg *src)
{
    bool dneg = dest->l.upper & 0x8000;
    bool sneg = src->l.upper & 0x8000;
    uint16_t dexp = dest->l.upper & 0x7fff;
    uint16_t sexp = src->l.upper & 0x7fff;
    bool less;

    if (dexp == 0 && sexp == 0) {
        return FPSR_CC_Z;
    }
    if (dneg != sneg) {
        return dneg ? FPSR_CC_N : 0;
    }
    if (dexp == sexp && dest->l.lower == src->l.lower) {
        return FPSR_CC_Z;
    }
    /* Same sign: order by magnitude, the other way round when negative. */
    less = dexp != sexp ? dexp < sexp : dest->l.lower < src->l.lower;
    return less != dneg ? FPSR_CC_N : 0;
}

/*
 * FPSR for an instruction the x87 completed: condition codes cc, and the
 * sticky softfloat inexact flag that exact mode's operation would raise.
 * Inexact is the only exception left.
 */
static inline void ffast_set_fpsr(CPUM68KState *env, uint32_t cc,
                                  bool inexact)
{
    env->fpsr = (env->fpsr & ~FPSR_CC_MASK) | cc;
    if (inexact) {
        float_raise(float_flag_inexact, &env->fp_status);
    }
}

/* A .B/.W/.L operand, sign-extended as exact mode loads it. */
static inline int32_t ffast_raw_int(uint64_t raw, int opsize)
{
    switch (opsize) {
    case OS_BYTE:
        return (int8_t)raw;
    case OS_WORD:
        return (int16_t)raw;
    default:
        return (int32_t)raw;
    }
}

/*
 * Loads a .B/.W/.L/.S/.D operand that is not an infinity or a NaN into *r,
 * exactly.  The x87 raises DE when it normalizes a denormal .S or .D; the
 * flag is cleared again, as nothing was lost.
 */
static void ffast_convert(FPReg *r, uint64_t raw, int opsize)
{
    switch (opsize) {
    case OS_SINGLE: {
        uint32_t v = raw;

        asm volatile("flds %[v]\n\t" "fstpt %[r]"
                     : [r] "=m"(*r) : [v] "m"(v) : "st");
        if (!(v & 0x7f800000) && (v & 0x007fffff)) {
            ffast_clear_flags();
        }
        break;
    }
    case OS_DOUBLE:
        asm volatile("fldl %[v]\n\t" "fstpt %[r]"
                     : [r] "=m"(*r) : [v] "m"(raw) : "st");
        if (!(raw & 0x7ff0000000000000ull) && (raw & 0x000fffffffffffffull)) {
            ffast_clear_flags();
        }
        break;
    default: {
        int32_t v = ffast_raw_int(raw, opsize);

        asm volatile("fildl %[v]\n\t" "fstpt %[r]"
                     : [r] "=m"(*r) : [v] "m"(v) : "st");
        break;
    }
    }
}

/* Computes an x87 kind into *r; returns the status word. */
static uint16_t ffast_compute(FFastKind k, FPReg *r, const FPReg *dest,
                              const FPReg *src)
{
    uint16_t sw;

    switch (k) {
    case FFAST_ROUND:
        FFAST_MONADIC(X87_ROUND, r, src, sw);
        break;
    case FFAST_INT:
        /*
         * floatx80_round_to_int() rounds to an integer, then to the
         * precision; frndint ignores the precision control.
         */
        FFAST_MONADIC("frndint\n\t" X87_ROUND, r, src, sw);
        break;
    case FFAST_SQRT:
        FFAST_MONADIC("fsqrt", r, src, sw);
        break;
    case FFAST_ABS:
        FFAST_MONADIC("fabs\n\t" X87_ROUND, r, src, sw);
        break;
    case FFAST_NEG:
        FFAST_MONADIC("fchs\n\t" X87_ROUND, r, src, sw);
        break;
    case FFAST_ADD:
        FFAST_DYADIC("fadd", r, dest, src, sw);
        break;
    case FFAST_SUB:
        FFAST_DYADIC("fsub", r, dest, src, sw);
        break;
    case FFAST_MUL:
        FFAST_DYADIC("fmul", r, dest, src, sw);
        break;
    case FFAST_DIV:
        FFAST_DYADIC("fdiv", r, dest, src, sw);
        break;
    default:
        g_assert_not_reached();
    }
    return sw;
}

/* Writes an accepted result r to dest, and FPSR. */
static inline void ffast_accept(CPUM68KState *env, FPReg *dest,
                                const FPReg *r, bool inexact)
{
    uint32_t cc = ffast_cc(r);

    dest->l.upper = r->l.upper;
    dest->l.lower = r->l.lower;
    ffast_set_fpsr(env, cc, inexact);
}

/*
 * The x87 part of an attempt: kind k, computing a OP b or OP b, under the
 * control word cw, which the caller has loaded; inexact says whether
 * anything before the operation was.  Returns false, having written nothing
 * to the guest, when the exact path has to do the instruction.
 */
static bool ffast_try_x87(CPUM68KState *env, FPReg *dest, const FPReg *a,
                          const FPReg *b, FFastKind k, uint16_t cw,
                          bool inexact)
{
    uint16_t sw;
    FPReg r;

    sw = ffast_compute(k, &r, a, b);
    if (sw & X87_NOT_INEXACT) {
        ffast_clear_flags();
        return false;
    }
    if (!ffast_result_ok(&r, ffast_zero_ok(k, a, b)) ||
        !ffast_range_ok(&r, cw)) {
        return false;
    }
    inexact |= ffast_inexact(k, &r, a, b, ffast_prec(cw));
    /*
     * A rounded result in the lowest normal binade: whether that underflowed
     * depends on whether tininess is detected before or after rounding, so
     * it is left to the exact path, whichever convention fp_status has.
     */
    if ((r.l.upper & 0x7fff) == 1 && inexact) {
        return false;
    }
    ffast_accept(env, dest, &r, inexact);
    return true;
}

/*
 * Rounds the significand of a normal r to 24 bits in the direction of cw's
 * rounding control, keeping the extended exponent.  False when that carries
 * into the top binade, which is left to the exact path as an overflow would
 * be.
 */
static bool ffast_round24(FPReg *r, uint16_t cw)
{
    uint64_t m = r->l.lower, low = m & ~FFAST_SGL_MASK, half = 1ull << 39;
    bool neg = r->l.upper & 0x8000;
    bool up;

    if (!low) {
        return true;
    }
    switch (cw & X87_RC_MASK) {
    case X87_RC_DOWN:
        up = neg;
        break;
    case X87_RC_UP:
        up = !neg;
        break;
    case X87_RC_CHOP:
        up = false;
        break;
    default:
        /* to nearest, ties to the even significand */
        up = low > half || (low == half && (m & (1ull << 40)));
        break;
    }
    m &= FFAST_SGL_MASK;
    if (up) {
        m += 1ull << 40;
        if (!m) {
            if ((r->l.upper & 0x7fff) + 1 >= 0x7ffe) {
                return false;
            }
            m = 1ull << 63;
            r->l.upper++;
        }
    }
    r->l.lower = m;
    return true;
}

/*
 * FSGLMUL of the operands a and b, already truncated to 24 bits.  Their
 * significands multiply exactly in 48 bits, so the x87 runs under FPCR's own
 * word, which the caller has loaded: the product is exact under double or
 * extended precision, and already rounded under single.  It is then rounded
 * to 24 bits here, under FPCR's rounding mode, which is what
 * helper_fsglmul()'s floatx80_mul under precision_s does.  So FSGLMUL needs no
 * precision switch, and no fldcw between it and the ordinary operations.
 * The result is held to the single range, as ffast_range_ok() says; the
 * tininess rule is ffast_try_x87()'s.
 */
static bool ffast_try_sglmul(CPUM68KState *env, FPReg *dest, const FPReg *a,
                             const FPReg *b, bool inexact)
{
    uint16_t cw = env->fast_fpu_cw;
    uint16_t sw;
    bool lost;
    FPReg r;

    FFAST_DYADIC("fmul", &r, a, b, sw);
    if (sw & X87_NOT_INEXACT) {
        ffast_clear_flags();
        return false;
    }
    if (!ffast_result_ok(&r, ffast_zero_ok(FFAST_MUL, a, b))) {
        return false;
    }
    lost = ffast_inexact(FFAST_MUL, &r, a, b, 24);
    if ((lost && !ffast_round24(&r, cw)) ||
        !ffast_range_ok(&r, (cw & ~X87_PC_MASK) | X87_PC_S)) {
        return false;
    }
    inexact |= lost;
    if ((r.l.upper & 0x7fff) == 1 && inexact) {
        return false;
    }
    ffast_accept(env, dest, &r, inexact);
    return true;
}

/*
 * The transcendentals run on the C library's long double functions.  They
 * assume the default control word, so it is loaded first, at its own site.
 * The result is then rounded to FPCR's precision by the x87 under FPCR's
 * word.  That rounds twice, once to 64 bits and once to FPCR's format, which
 * the m68k FPSP does not do; the library's own error is larger anyway, and
 * fporacle.py holds the sum to 2 ulp.  At extended precision (and round to
 * nearest, the only mode taken, below) FPCR's word is the default one: the
 * rounding would change nothing and is skipped, and no fldcw is needed
 * between the library calls and the other operations.
 *
 * FTENTOX stays exact on Windows: mingw-w64 has no exp10l, and its
 * powl(10, x) was measured 6 ulp off.
 *
 * Only round to nearest is taken: the library rounds to nearest, so under a
 * directed mode (RZ, RM, RP) its result would not be on the side FPCR names.
 * Only a nonzero normal operand is taken, and only a result that is finite
 * and normal in the range of its precision, with the x87 raising nothing but
 * inexact.  Exact mode's exact special cases (a zero or an infinity in or
 * out, an argument outside the real domain) all run there.  Every result
 * taken is flagged inexact, as the FPSP flags every result it computes.
 */

/* The operand is inside the real domain of opmode's function. */
static bool ffast_libm_domain(uint32_t opmode, const FPReg *x)
{
    bool neg = x->l.upper & 0x8000;
    int exp = x->l.upper & 0x7fff;
    bool lt1 = exp < FFAST_BIAS;
    bool le1 = lt1 || (exp == FFAST_BIAS && x->l.lower == 1ull << 63);

    switch (opmode) {
    case 0x0c: case 0x1c:               /* FASIN, FACOS */
        return le1;
    case 0x0d:                          /* FATANH */
        return lt1;
    case 0x06:                          /* FLOGNP1 */
        return !neg || lt1;
    case 0x14: case 0x15: case 0x16:    /* FLOGN, FLOG10, FLOG2 */
        return !neg;
    default:
        return true;
    }
}

static long double ffast_libm(uint32_t opmode, long double x)
{
    switch (opmode) {
    case 0x02: return sinhl(x);
    case 0x06: return log1pl(x);
    case 0x08: return expm1l(x);
    case 0x09: return tanhl(x);
    case 0x0a: return atanl(x);
    case 0x0c: return asinl(x);
    case 0x0d: return atanhl(x);
    case 0x0e: return sinl(x);
    case 0x0f: return tanl(x);
    case 0x10: return expl(x);
    case 0x11: return exp2l(x);
#ifndef _WIN32
    case 0x12: return powl(10.0L, x);
#endif
    case 0x14: return logl(x);
    case 0x15: return log10l(x);
    case 0x16: return log2l(x);
    case 0x19: return coshl(x);
    case 0x1c: return acosl(x);
    case 0x1d: return cosl(x);
    default:
        g_assert_not_reached();
    }
}

/*
 * Rounds r, a result of the library, to FPCR's precision and mode and checks
 * what exact mode would do: false unless the result is normal, inside the
 * range of the precision, and not in the lowest binade (tininess, as in
 * ffast_try_x87()).
 */
static bool ffast_libm_round(CPUM68KState *env, FPReg *r)
{
    uint16_t cw = env->fast_fpu_cw;

    if (cw != X87_CW_DEFAULT) {
        uint16_t sw;

        FFAST_LOAD_CW(cw, "fpcr");
        FFAST_MONADIC(X87_ROUND, r, r, sw);
        if (sw & X87_NOT_INEXACT) {
            ffast_clear_flags();
            return false;
        }
    }
    return (r->l.upper & 0x7fff) > 1 && ffast_result_ok(r, false) &&
           ffast_range_ok(r, cw);
}

/*
 * One or two results (sine and cosine) of the library function(s) of opmode
 * for the operand src, rounded for FPCR; none of them is written to the
 * guest.  False when the exact path has to do the instruction.
 */
static bool ffast_libm_compute(CPUM68KState *env, FPReg *r0, FPReg *r1,
                               const FPReg *src, uint32_t opmode)
{
    union { long double v; FPReg f; } x = { .f = *src }, y0, y1;
    uint16_t sw;

    if ((env->fast_fpu_cw & X87_RC_MASK) || !(src->l.upper & 0x7fff) ||
        !ffast_libm_domain(opmode, src)) {
        return false;
    }
    FFAST_LOAD_CW(X87_CW_DEFAULT, "libm");
    if (r1) {
        y0.v = sinl(x.v);
        y1.v = cosl(x.v);
        asm volatile("" : "+m"(y1.v));
    } else {
        y0.v = ffast_libm(opmode, x.v);
    }
    /* the results are stored, so the status word has their flags */
    asm volatile("" : "+m"(y0.v));
    asm volatile("fnstsw %0" : "=a"(sw));
    if (sw & X87_NOT_INEXACT) {
        ffast_clear_flags();
        return false;
    }
    *r0 = y0.f;
    if (!ffast_libm_round(env, r0)) {
        return false;
    }
    if (r1) {
        *r1 = y1.f;
        return ffast_libm_round(env, r1);
    }
    return true;
}

static bool ffast_try_libm(CPUM68KState *env, FPReg *dest, const FPReg *src,
                           uint32_t opmode)
{
    FPReg r;

    if (!ffast_libm_compute(env, &r, NULL, src, opmode)) {
        return false;
    }
    ffast_accept(env, dest, &r, true);
    return true;
}

/*
 * FSINCOS: the cosine to FP(opmode & 7) first, then the sine to the
 * destination, as helper_fsincos() does, so the sine wins when they are the
 * same register; FPSR is the sine's.
 */
static bool ffast_try_sincos(CPUM68KState *env, FPReg *dest, const FPReg *src,
                             uint32_t opmode)
{
    FPReg sine, cosine, *c = &env->fregs[opmode & 7];

    if (!ffast_libm_compute(env, &sine, &cosine, src, opmode)) {
        return false;
    }
    c->l.upper = cosine.l.upper;
    c->l.lower = cosine.l.lower;
    ffast_accept(env, dest, &sine, true);
    return true;
}

/*
 * The attempt at opmode, of kind k (not FFAST_EXACT), with src zero or
 * normal; inexact says whether src's conversion from a raw operand was.
 * Returns false, having written nothing to the guest, when the exact path
 * has to do the instruction.
 */
static bool ffast_try(CPUM68KState *env, FPReg *dest, const FPReg *src,
                      uint32_t opmode, FFastKind k, bool inexact)
{
    uint16_t cw;
    FPReg a, b;

    if ((k == FFAST_CMP || k >= FFAST_ADD) && !ffast_operand_ok(dest)) {
        return false;
    }
    switch (k) {
    case FFAST_TST:
        ffast_set_fpsr(env, ffast_cc(src), inexact);
        return true;
    case FFAST_CMP:
        ffast_set_fpsr(env, ffast_compare(dest, src), inexact);
        return true;
    case FFAST_MOVE:
        ffast_accept(env, dest, src, inexact);
        return true;
    case FFAST_LIBM:
        return ffast_try_libm(env, dest, src, opmode);
    case FFAST_SINCOS:
        return ffast_try_sincos(env, dest, src, opmode);
    case FFAST_SGLMUL:
    case FFAST_SGLDIV:
        /*
         * helper_fsglmul() and helper_fsgldiv() first round both operands to
         * single precision towards zero, then operate in single precision
         * with FPCR's rounding mode.  An operand outside the single range is
         * left to the exact path, as ffast_range_ok() says.
         */
        cw = (env->fast_fpu_cw & ~X87_PC_MASK) | X87_PC_S;
        if (!ffast_range_ok(dest, cw) || !ffast_range_ok(src, cw)) {
            return false;
        }
        inexact |= ffast_lost(dest->l.lower, 24) ||
                   ffast_lost(src->l.lower, 24);
        a.l.upper = dest->l.upper;
        a.l.lower = dest->l.lower & FFAST_SGL_MASK;
        b.l.upper = src->l.upper;
        b.l.lower = src->l.lower & FFAST_SGL_MASK;
        if (k == FFAST_SGLMUL) {
            FFAST_LOAD_CW(env->fast_fpu_cw, "fpcr");
            return ffast_try_sglmul(env, dest, &a, &b, inexact);
        }
        FFAST_LOAD_CW(cw, "fsgldiv");
        return ffast_try_x87(env, dest, &a, &b, FFAST_DIV, cw, inexact);
    default:
        cw = ffast_op_cw(env, opmode);
        if (opmode & 0x40) {
            if (opmode & 4) {
                FFAST_LOAD_CW(cw, "fd");
            } else {
                FFAST_LOAD_CW(cw, "fs");
            }
        } else if (opmode == 0x03) {
            FFAST_LOAD_CW(cw, "fintrz");
        } else {
            FFAST_LOAD_CW(cw, "fpcr");
        }
        return ffast_try_x87(env, dest, dest, src, k, cw, inexact);
    }
}

/* The exact-path bodies, by opmode, as DISAS_INSN(fpu) emits them. */
typedef void FFastExact1(CPUM68KState *, FPReg *, FPReg *);
typedef void FFastExact2(CPUM68KState *, FPReg *, FPReg *, FPReg *);

static FFastExact1 * const ffast_exact1[0x80] = {
    [0x01] = helper_firound,  [0x02] = helper_fsinh,
    [0x03] = helper_fitrunc,  [0x04] = helper_fsqrt,
    [0x06] = helper_flognp1,  [0x08] = helper_fetoxm1,
    [0x09] = helper_ftanh,    [0x0a] = helper_fatan,
    [0x0c] = helper_fasin,    [0x0d] = helper_fatanh,
    [0x0e] = helper_fsin,     [0x0f] = helper_ftan,
    [0x10] = helper_fetox,    [0x11] = helper_ftwotox,
    [0x12] = helper_ftentox,  [0x14] = helper_flogn,
    [0x15] = helper_flog10,   [0x16] = helper_flog2,
    [0x18] = helper_fabs,     [0x19] = helper_fcosh,
    [0x1a] = helper_fneg,     [0x1c] = helper_facos,
    [0x1d] = helper_fcos,     [0x1e] = helper_fgetexp,
    [0x1f] = helper_fgetman,
    [0x40] = helper_fsround,  [0x41] = helper_fssqrt,
    [0x44] = helper_fdround,  [0x45] = helper_fdsqrt,
    [0x58] = helper_fsabs,    [0x5a] = helper_fsneg,
    [0x5c] = helper_fdabs,    [0x5e] = helper_fdneg,
};

static FFastExact2 * const ffast_exact2[0x80] = {
    [0x20] = helper_fdiv,     [0x21] = helper_fmod,
    [0x22] = helper_fadd,     [0x23] = helper_fmul,
    [0x24] = helper_fsgldiv,  [0x25] = helper_frem,
    [0x26] = helper_fscale,   [0x27] = helper_fsglmul,
    [0x28] = helper_fsub,
    [0x60] = helper_fsdiv,    [0x62] = helper_fsadd,
    [0x63] = helper_fsmul,    [0x64] = helper_fddiv,
    [0x66] = helper_fdadd,    [0x67] = helper_fdmul,
    [0x68] = helper_fssub,    [0x6c] = helper_fdsub,
};

/*
 * What exact mode does for opmode: the op helper, then ftst on the result,
 * except FCMP and FTST, which set FPSR themselves.  The dyadic helpers take
 * the source first and compute dest OP src.
 */
static void ffast_exact_op(CPUM68KState *env, FPReg *dest, FPReg *src,
                           uint32_t opmode)
{
    switch (opmode) {
    case 0x00:
        dest->l.upper = src->l.upper;
        dest->l.lower = src->l.lower;
        break;
    case 0x30 ... 0x37:
        helper_fsincos(env, dest, &env->fregs[opmode & 7], src);
        break;
    case 0x38:
        helper_fcmp(env, src, dest);
        return;
    case 0x3a:
        helper_ftst(env, src);
        return;
    default:
        if (ffast_exact1[opmode]) {
            ffast_exact1[opmode](env, dest, src);
        } else {
            g_assert(ffast_exact2[opmode]);
            ffast_exact2[opmode](env, dest, src, dest);
        }
        break;
    }
    helper_ftst(env, dest);
}

void HELPER(ffast_op)(CPUM68KState *env, FPReg *dest, FPReg *src,
                      uint32_t opmode)
{
    FFastKind k = ffast_kind(opmode);

    if (k != FFAST_EXACT && ffast_operand_ok(src) &&
        ffast_try(env, dest, src, opmode, k, false)) {
        return;
    }
    ffast_exact_op(env, dest, src, opmode);
}

void HELPER(ffast_op_raw)(CPUM68KState *env, FPReg *dest, uint64_t raw,
                          uint32_t desc)
{
    uint32_t opmode = desc & 0x7f;
    int opsize = desc >> 8;
    FFastKind k = ffast_kind(opmode);
    FPReg *src = &env->fp_result;

    if (k != FFAST_EXACT && ffast_raw_ok(raw, opsize)) {
        uint16_t cw = env->fast_fpu_cw;
        bool inexact = false;
        uint16_t sw = 0;
        FPReg conv;

        ffast_convert(&conv, raw, opsize);
        /*
         * Exact mode's conversion is rounded to FPCR's precision.  Only a .L
         * or .D operand can have more significant bits than single precision
         * holds, and none has more than double.
         */
        if (ffast_lost(conv.l.lower, ffast_prec(cw))) {
            inexact = true;
            FFAST_LOAD_CW(cw, "conv");
            FFAST_MONADIC(X87_ROUND, &conv, &conv, sw);
        }
        if (sw & X87_NOT_INEXACT) {
            ffast_clear_flags();
        } else if (ffast_range_ok(&conv, cw) &&
                   ffast_try(env, dest, &conv, opmode, k, inexact)) {
            return;
        }
    }

    /* As gen_load_fp() converts into fp_result, then the instruction. */
    switch (opsize) {
    case OS_SINGLE:
        helper_extf32(env, src, (uint32_t)raw);
        break;
    case OS_DOUBLE:
        helper_extf64(env, src, raw);
        break;
    default:
        helper_exts32(env, src, ffast_raw_int(raw, opsize));
        break;
    }
    ffast_exact_op(env, dest, src, opmode);
}

uint64_t HELPER(ffast_store)(CPUM68KState *env, FPReg *src, uint32_t opsize)
{
    uint64_t v;

    if (ffast_operand_ok(src)) {
        uint16_t cw = env->fast_fpu_cw;
        bool zero = !(src->l.upper & 0x7fff);
        /*
         * An integer store rounding towards zero (compiled float-to-int
         * code sets FPCR's rounding mode just for it) uses fisttp, which
         * truncates whatever the control word says: no fldcw.
         */
        bool trunc = opsize <= OS_LONG &&
                     (cw & X87_RC_MASK) == X87_RC_CHOP &&
                     (cpuinfo & CPUINFO_SSE3);
        bool inexact;
        uint16_t sw;

        if (!trunc) {
            ffast_load_store_cw(cw);
        }
        switch (opsize) {
        case OS_SINGLE: {
            uint32_t f;

            asm volatile("fldt %[s]\n\t" "fstps %[v]\n\t" "fnstsw %[w]"
                         : [v] "=m"(f), [w] "=a"(sw) : [s] "m"(*src) : "st");
            inexact = ffast_lost(src->l.lower, 24);
            if (!(sw & X87_NOT_INEXACT) &&
                ffast_out_ok(extract32(f, 23, 8), 0xff, zero, inexact)) {
                ffast_set_fpsr(env, ffast_cc(src), inexact);
                return f;
            }
            break;
        }
        case OS_DOUBLE: {
            uint64_t d;

            asm volatile("fldt %[s]\n\t" "fstpl %[v]\n\t" "fnstsw %[w]"
                         : [v] "=m"(d), [w] "=a"(sw) : [s] "m"(*src) : "st");
            inexact = ffast_lost(src->l.lower, 53);
            if (!(sw & X87_NOT_INEXACT) &&
                ffast_out_ok(extract64(d, 52, 11), 0x7ff, zero, inexact)) {
                ffast_set_fpsr(env, ffast_cc(src), inexact);
                return d;
            }
            break;
        }
        default: {
            /*
             * .B and .W store the low bits of the int32, as in exact mode.
             * INT32_MIN is also what an out-of-range source gives.
             */
            int32_t i;

            if (trunc) {
                asm volatile("fldt %[s]\n\t" "fisttpl %[v]\n\t" "fnstsw %[w]"
                             : [v] "=m"(i), [w] "=a"(sw) : [s] "m"(*src)
                             : "st");
            } else {
                asm volatile("fldt %[s]\n\t" "fistpl %[v]\n\t" "fnstsw %[w]"
                             : [v] "=m"(i), [w] "=a"(sw) : [s] "m"(*src)
                             : "st");
            }
            if (!(sw & X87_NOT_INEXACT) && i != INT32_MIN) {
                ffast_set_fpsr(env, ffast_cc(src), ffast_int_inexact(src, 64));
                return (uint32_t)i;
            }
            break;
        }
        }
        if (sw & X87_NOT_INEXACT) {
            ffast_clear_flags();
        }
    }

    /* The conversion helper gen_store_fp() calls, then ftst on the source. */
    switch (opsize) {
    case OS_SINGLE:
        v = helper_redf32(env, src);
        break;
    case OS_DOUBLE:
        v = helper_redf64(env, src);
        break;
    default:
        v = (uint32_t)helper_reds32(env, src);
        break;
    }
    helper_ftst(env, src);
    return v;
}

#endif
