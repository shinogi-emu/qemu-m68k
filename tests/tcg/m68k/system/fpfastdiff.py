#!/usr/bin/env python3
"""
Fast-against-exact differential for the 68881 arithmetic.

The guest runs every case once and dumps, per case, the 12-byte result (or the
value stored to memory) and the FPSR.  The harness runs the same binary with
-global m68k-cpu.fast-fpu=off and =on and compares the two dumps: they must be
equal bit for bit, result and whole FPSR.  The transcendentals are not here;
fporacle.py covers them.

The same guest runs on the 68040 and the 68060.  The 68060 has no FGETEXP,
FGETMAN, FMOD, FREM or FSCALE: they take the F-line exception, for the
guest's 68060 software package to emulate.  The guest's F-line handler counts
those traps and resumes after the instruction (the frame's PC is the next
instruction), so such a case records its operand unchanged.  The count
follows the records in the dump; both modes must trap the same instructions.
Any other exception prints "!" and stops the guest.

Usage:
    fpfastdiff.py emit  <out.S>               write the guest
    fpfastdiff.py check [--cpu m68040|m68060] <exact.dump> <fast.dump>

check prints every difference as "op mode prec operands: exact=... fast=..."
and exits 1 if there is one, if the dumps differ in length, or if the trap
counts differ from each other or, with --cpu, from that CPU's.

The case list is a pure function of this file (fixed seed), so check rebuilds
it to name the cases instead of the guest carrying names.
"""

import random
import sys

SEED = 0x5eed

# Each case is one 16-byte record: 12 bytes of result, then the FPSR.  The
# records are followed by the 4-byte F-line trap count.
REC = 16
TRAILER = 4

# Not in the 68060's FPU: F-line, for the 68060 software package.
UNIMP_060 = {"fgetexp", "fgetman", "fmod", "frem", "fscale"}

MODES = ["RN", "RZ", "RM", "RP"]
PRECS = ["X", "S", "D"]

# FPCR bit 13: OPERR enable.  Normal operands never raise OPERR, so the
# bare-metal guest (which has no FP exception handler) never takes the trap.
OPERR_ENABLE = 0x2000


def fpcr_value(prec, mode, enable=0):
    return enable | (PRECS.index(prec) << 6) | (MODES.index(mode) << 4)


# ---------------------------------------------------------------------------
# operands
# ---------------------------------------------------------------------------

def build_operands():
    rnd = random.Random(SEED)

    def normal(lo, hi):
        se = rnd.randint(lo, hi) | (rnd.getrandbits(1) << 15)
        return (se, (1 << 63) | rnd.getrandbits(63))

    normals = [normal(0x3f00, 0x40ff) for _ in range(64)]
    # Mid-range values: the ones whose integer conversions, FMOD/FREM/FSCALE
    # results and FINT roundings are not trivially zero or out of range.
    mid = [normal(0x3ffc, 0x401e) for _ in range(32)]

    big = (0x7ffe, (1 << 64) - 1)
    small = (0x0001, 1 << 63)
    specials = [
        ("+0", (0x0000, 0)),
        ("-0", (0x8000, 0)),
        ("+inf", (0x7fff, 0)),
        ("-inf", (0xffff, 0)),
        ("qnan", (0x7fff, 0xc000000000001234)),
        ("-qnan", (0xffff, 0xc000000000000042)),
        ("snan", (0x7fff, 0x8000000000000777)),
        ("-snan", (0xffff, 0xa000000000000001)),
        ("denormal", (0x0000, 0x0000123456789abc)),
        ("-denormal", (0x8000, 0x0000000000000001)),
        ("unnormal", (0x3fff, 0x4000000000000000)),
        ("pseudo-denormal", (0x0000, 0x8000000000000000)),
        ("max", big),
        ("min", small),
        ("1.0", (0x3fff, 1 << 63)),
        ("-1.0", (0xbfff, 1 << 63)),
        ("0.5", (0x3ffe, 1 << 63)),
        ("2.0", (0x4000, 1 << 63)),
    ]
    return normals, mid, specials, big, small


def build_tables():
    """Everything the guest and the checker both derive from the seed."""
    normals, mid, specials, big, small = build_operands()
    rnd = random.Random(SEED + 1)

    normal_ops = [("n%d" % i, v) for i, v in enumerate(normals)] \
        + [("m%d" % i, v) for i, v in enumerate(mid)]
    all_ops = normal_ops + specials

    # Pairs with a sum that overflows and a product that underflows, plus the
    # directed edge pairs the specials cross products do not reach.
    ovf = (0x7ffd, 1 << 63)
    unf = (0x0002, 1 << 63)
    edge = [
        ("ovf", (big, big)), ("ovf2", (ovf, ovf)),
        ("ovf3", (big, (0xfffe, (1 << 64) - 1))),   # FSUB: max - -max
        ("unf", (small, small)), ("unf2", (unf, unf)),
        ("unf3", (small, (0x3ffe, 1 << 63))),
        ("unf4", ((0x0000, 0x4000000000000000), (0x3ffe, 1 << 63))),
        ("unf5", (small, (0x3fff, (1 << 63) | 1))),
    ]
    edge = [("%s" % n, p) for n, p in edge]

    pairs_all = []
    for an, a in specials:
        for bn, b in specials:
            pairs_all.append(("%s,%s" % (an, bn), (a, b)))
    for an, a in specials:
        bn, b = normal_ops[rnd.randrange(len(normal_ops))]
        pairs_all.append(("%s,%s" % (an, bn), (a, b)))
        pairs_all.append(("%s,%s" % (bn, an), (b, a)))
    pairs_norm = []
    for i in range(len(normal_ops)):
        an, a = normal_ops[i]
        bn, b = normal_ops[(i * 7 + 3) % len(normal_ops)]
        pairs_norm.append(("%s,%s" % (an, bn), (a, b)))
    pairs_all += pairs_norm + edge

    # Memory inputs and outputs.  Slots are 12 bytes; the value sits at the
    # start of the slot (big-endian).
    mem_in = {
        "B": [0, 1, 0xff, 0x7f, 0x80, 0x55],
        "W": [0, 1, 0xffff, 0x7fff, 0x8000, 0x1234],
        "L": [0, 1, 0xffffffff, 0x7fffffff, 0x80000000, 0x075bcd15],
        "S": [0x3f800000, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000,
              0x7fa00000, 0x00000001, 0x7f7fffff, 0x3dcccccd, 0x00800000],
        "D": [0x3ff0000000000000, 0x8000000000000000, 0x7ff0000000000000,
              0x7ff8000000000000, 0x7ff4000000000000, 0x0000000000000001,
              0x7fefffffffffffff, 0x3fb999999999999a, 0x0010000000000000,
              0xc00921fb54442d18],
    }
    return normal_ops, all_ops, specials, pairs_all, pairs_norm, mem_in


# Register-to-register ops: (name, opmode, arity).
UNARY = [
    ("fmove", 0x00), ("fint", 0x01), ("fintrz", 0x03), ("fsqrt", 0x04),
    ("fabs", 0x18), ("fneg", 0x1a), ("ftst", 0x3a),
    ("fgetexp", 0x1e), ("fgetman", 0x1f),
    ("fsmove", 0x40), ("fdmove", 0x44), ("fssqrt", 0x41), ("fdsqrt", 0x45),
    ("fsabs", 0x58), ("fdabs", 0x5c), ("fsneg", 0x5a), ("fdneg", 0x5e),
]
BINARY = [
    ("fadd", 0x22), ("fsub", 0x28), ("fmul", 0x23), ("fdiv", 0x20),
    ("fcmp", 0x38), ("fsglmul", 0x27), ("fsgldiv", 0x24),
    ("fmod", 0x21), ("frem", 0x25), ("fscale", 0x26),
    ("fsadd", 0x62), ("fdadd", 0x66), ("fssub", 0x68), ("fdsub", 0x6c),
    ("fsmul", 0x63), ("fdmul", 0x67), ("fsdiv", 0x60), ("fddiv", 0x64),
]
# These can raise OPERR on normal operands, so the OPERR-enable pass drops
# them: the square roots on a negative operand, and FREM because the helper
# converts the quotient to int32 with the live fp_status, which raises invalid
# whenever the quotient does not fit.
OPERR_PRONE = {"fsqrt", "fssqrt", "fdsqrt", "frem"}

# Memory formats: name -> (format code, bytes)
FMT = {"L": 0, "S": 1, "X": 2, "W": 4, "D": 5, "B": 6}


class Group:
    """One loop in the guest: a fixed op, FPCR and table of cases."""

    def __init__(self, label, fpcr, kind, op, ext, table, stride):
        self.label = label
        self.fpcr = fpcr
        self.kind = kind            # unary | binary | memin | memout
        self.op = op
        self.ext = ext              # the extension word
        self.table = table          # key of the shared table in TABLES
        self.cases = TABLES[table]  # [(operand text, table words)]
        self.stride = stride


def other_fpcr(fpcr):
    """An FPCR with a different precision and rounding mode and no enables."""
    prec = ((fpcr >> 6) & 3) + 1
    prec = 0 if prec > 2 else prec
    mode = (((fpcr >> 4) & 3) + 1) & 3
    return (prec << 6) | (mode << 4)


def pass_name(prec, mode, enable):
    return "%s %s%s" % (mode, prec, " OPERR-en" if enable else "")


TABLES = {}


def build_groups():
    TABLES.clear()
    normal_ops, all_ops, _specials, pairs_all, pairs_norm, mem_in = build_tables()
    groups = []

    def words(se, sig):
        return [se << 16, sig >> 32, sig & 0xffffffff]

    def un_cases(ops):
        return [("%s=%04x:%016x" % (n, v[0], v[1]), words(*v)) for n, v in ops]

    def bin_cases(pairs):
        return [("%s=%04x:%016x,%04x:%016x" % (n, a[0], a[1], b[0], b[1]),
                 words(*a) + words(*b)) for n, (a, b) in pairs]

    TABLES["un_norm"] = un_cases(normal_ops)
    TABLES["un_all"] = un_cases(all_ops)
    TABLES["bin_norm"] = bin_cases(pairs_norm)
    TABLES["bin_all"] = bin_cases(pairs_all)
    for f, vals in mem_in.items():
        cs = []
        nbytes = {"B": 1, "W": 2, "L": 4, "S": 4, "D": 8}[f]
        for v in vals:
            data = v.to_bytes(nbytes, "big").ljust(12, b"\0")
            cs.append(("%s=%x" % (f, v),
                       [int.from_bytes(data[i:i + 4], "big")
                        for i in range(0, 12, 4)]))
        TABLES["in_" + f] = cs

    passes = [(p, m, 0, False) for m in MODES for p in PRECS]
    passes += [(p, "RN", OPERR_ENABLE, True) for p in PRECS]

    for prec, mode, enable, restricted in passes:
        fpcr = fpcr_value(prec, mode, enable)
        pname = pass_name(prec, mode, enable)
        utab = "un_norm" if restricted else "un_all"
        btab = "bin_norm" if restricted else "bin_all"
        for name, om in UNARY:
            if restricted and name in OPERR_PRONE:
                continue
            groups.append(Group(pname, fpcr, "unary", name, om,
                                utab, 12))
        for name, om in BINARY:
            if restricted and name in OPERR_PRONE:
                continue
            groups.append(Group(pname, fpcr, "binary", name, 0x0400 | om,
                                btab, 24))
        if restricted:
            continue
        for f in mem_in:
            groups.append(Group(pname, fpcr, "memin", "fmove.%s in" % f.lower(),
                                0x4000 | (FMT[f] << 10), "in_" + f, 12))
        for f in "BWLSDX":
            groups.append(Group(pname, fpcr, "memout",
                                "fmove.%s out" % f.lower(),
                                0x6000 | (FMT[f] << 10), "un_all", 12))
    return groups


def total_cases(groups):
    return sum(len(g.cases) for g in groups)


# ---------------------------------------------------------------------------
# guest
# ---------------------------------------------------------------------------

def emit(path):
    groups = build_groups()
    n = total_cases(groups)
    L = []
    A = L.append
    A("/*")
    A(" * Fast-against-exact FPU differential.")
    A(" *")
    A(" * GENERATED by tests/tcg/m68k/system/fpfastdiff.py -- do not edit.")
    A(" * One 16-byte record per case: 12 bytes of result, then the FPSR.  The")
    A(" * dump goes out as hex on the serial port; fpfastdiff.py check compares")
    A(" * a fast-fpu=off dump with a fast-fpu=on dump.  The count of F-line")
    A(" * traps (FPU instructions a 68060 does not implement) follows the")
    A(" * records.")
    A(" *")
    A(" * SPDX-License-Identifier: GPL-2.0-or-later")
    A(" */")
    A("")
    A("        .text")
    A("        .globl  _start")
    A("_start:")
    A("        lea     0x00800000,%sp")
    A("        lea     vectors,%a0")
    A("        move.l  %a0,%a1")
    A("        move.w  #255,%d0")
    A("0:      move.l  #unexpected,(%a1)+")
    A("        dbra    %d0,0b")
    A("        move.l  #line_f,11*4(%a0)")
    A("        movec   %a0,%vbr")
    A("        clr.l   traps")
    A("        lea     results,%a1")
    A("")
    for gi, g in enumerate(groups):
        A("        /* %s: %s, %d cases */" % (g.label, g.op, len(g.cases)))
        A("        lea     tbl_%s,%%a0" % g.table)
        # dbra counts the low word, so the count is capped at 65536
        A("        move.w  #%d,%%d0" % (len(g.cases) - 1))
        # Leave a different mode and precision in FPCR and run one op under
        # it, so a stale cached control state is the wrong one.  Then load,
        # clear, and write the case's FPCR: only the FPSR clear separates that
        # write from the op, with no branch in between.
        A("1:      fmove.l #0x%04x,%%fpcr" % other_fpcr(g.fpcr))
        A("        fmul.x  %fp0,%fp0")
        if g.kind == "binary":
            A("        fmove.x (%a0),%fp0")
            A("        fmove.x 12(%a0),%fp1")
        elif g.kind != "memin":
            A("        fmove.x (%a0),%fp0")
        A("        clr.l   (%a1)")
        A("        clr.l   4(%a1)")
        A("        clr.l   8(%a1)")
        A("        fmove.l #0x%04x,%%fpcr" % g.fpcr)
        A("        fmove.l #0,%fpsr")
        if g.kind == "memin":
            A("        .word   0xf210,0x%04x          /* fmove.<f> (%%a0),%%fp0 */"
              % g.ext)
            A("        fmove.l %fpsr,%d1")
            A("        fmove.x %fp0,(%a1)")
        elif g.kind == "memout":
            A("        .word   0xf211,0x%04x          /* fmove.<f> %%fp0,(%%a1) */"
              % g.ext)
            A("        fmove.l %fpsr,%d1")
        else:
            A("        .word   0xf200,0x%04x          /* %s */" % (g.ext, g.op))
            A("        fmove.l %fpsr,%d1")
            A("        fmove.x %fp0,(%a1)")
        A("        move.l  %d1,12(%a1)")
        A("        lea     16(%a1),%a1")
        A("        lea     %d(%%a0),%%a0" % g.stride)
        A("        dbra    %d0,1b")
        A("")
    A("        /* dump every record, then the trap count, as hex */")
    A("        lea     results,%a0")
    A("        move.l  #%d,%%d6" % (n * REC + TRAILER))
    A("2:      moveq   #0,%d0")
    A("        move.b  (%a0)+,%d0")
    A("        move.l  %d0,%d1")
    A("        lsr.l   #4,%d1")
    A("        bsr     nib")
    A("        move.l  %d0,%d1")
    A("        bsr     nib")
    A("        subq.l  #1,%d6")
    A("        bne     2b")
    A("        move.l  #2,0xff009004")
    A("        stop    #0x2700")
    A("nib:    andi.l  #0xf,%d1")
    A("        cmpi.l  #9,%d1")
    A("        ble     3f")
    A("        addi.l  #'a'-10,%d1")
    A("        bra     4f")
    A("3:      addi.l  #'0',%d1")
    A("4:      move.l  %d1,0xff008000")
    A("        rts")
    A("")
    A("        /* 68060 FPU unimplemented instruction: stacked PC is the next */")
    A("line_f: addq.l  #1,traps")
    A("        rte")
    A("unexpected:")
    A("        move.l  #'!',0xff008000")
    A("        move.l  #2,0xff009004")
    A("        stop    #0x2700")
    A("")
    A("        .data")
    A("        .balign 4")
    for key, cases in sorted(TABLES.items()):
        A("tbl_%s:" % key)
        for text, ws in cases:
            A("        .long   %s   /* %s */"
              % (", ".join("0x%08x" % w for w in ws), text))
    A("        .bss")
    A("        .balign 4")
    A("results:")
    A("        .space  %d" % (n * REC))
    A("traps:  .space  %d" % TRAILER)
    A("vectors:")
    A("        .space  1024")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")
    print("wrote %s: %d groups, %d cases" % (path, len(groups), n))
    per = {}
    for g in groups:
        key = g.kind
        per[key] = per.get(key, 0) + len(g.cases)
    print("  " + ", ".join("%s %d" % kv for kv in sorted(per.items())))


# ---------------------------------------------------------------------------
# checker
# ---------------------------------------------------------------------------

def load(path):
    return bytes.fromhex(open(path).read().strip())


def expected_traps(groups, cpu):
    if cpu == "m68060":
        return sum(len(g.cases) for g in groups if g.op in UNIMP_060)
    return 0


def check(exact_path, fast_path, cpu=None):
    exact, fast = load(exact_path), load(fast_path)
    groups = build_groups()
    want = total_cases(groups) * REC + TRAILER
    if len(exact) != want or len(fast) != want:
        print("dump length: exact=%d fast=%d expected=%d bytes"
              % (len(exact), len(fast), want))
        return 1
    etraps = int.from_bytes(exact[-TRAILER:], "big")
    ftraps = int.from_bytes(fast[-TRAILER:], "big")
    print("F-line traps: exact=%d fast=%d" % (etraps, ftraps))
    if etraps != ftraps:
        return 1
    if cpu is not None and etraps != expected_traps(groups, cpu):
        print("expected %d F-line traps on %s"
              % (expected_traps(groups, cpu), cpu))
        return 1
    bad = 0
    off = 0
    for g in groups:
        for text, _ws in g.cases:
            e, f = exact[off:off + REC], fast[off:off + REC]
            off += REC
            if e != f:
                bad += 1
                print("%s %s: %s: exact=%s/%s fast=%s/%s"
                      % (g.op, g.label, text, e[:12].hex(), e[12:].hex(),
                         f[:12].hex(), f[12:].hex()))
    print("%d cases, %d differ" % (total_cases(groups), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "emit":
        emit(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == "check":
        sys.exit(check(sys.argv[2], sys.argv[3]))
    elif (len(sys.argv) == 6 and sys.argv[1] == "check" and
          sys.argv[2] == "--cpu" and sys.argv[3] in ("m68040", "m68060")):
        sys.exit(check(sys.argv[4], sys.argv[5], sys.argv[3]))
    else:
        print(__doc__)
        sys.exit(2)
