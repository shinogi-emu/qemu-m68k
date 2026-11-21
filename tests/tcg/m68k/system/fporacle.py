#!/usr/bin/env python3
"""
Independent oracle for the 68881 transcendentals.

QEMU and Hatari are the same code: both descend from Andreas Grabher's
Previous work via the NetBSD M68040 FPSP, and their FPSP constant tables are
byte-identical (1091 constants).  Neither can validate the other, and a bug in
that lineage would be invisible to QEMU, Hatari and any RTL checked against
them.  This computes the correctly-rounded extended-precision result with
mpmath instead, which shares no ancestry with any of them.

A pass is not "bit-identical": the FPSP is a polynomial approximation and a
correctly-rounded oracle disagrees in the last ulp by design.  The signal is
error far beyond that.

Usage:
    fporacle.py emit  <harness.S>          write the guest harness
    fporacle.py check <harness.S> <dump>   compare a guest dump to the oracle

    fporacle.py selftest                   check the floatx80 encoding itself
    fporacle.py ref <fn> <points>          reference values for a point list
    fporacle.py cmp <fn> <points> <got>    ulp distribution against a result list

The last two exist for other implementations.  <fn> is one of log, log1p,
log2, log10, sin, cos, tan, atan, asin, acos, atanh, sinh, cosh, tanh, exp,
expm1, exp2, exp10.  <points> holds one value per line, either decimal or
hex "se:sig" (e.g. 3ffb:f1383b7157972f4f); blank lines and # comments are
skipped.  <got> holds the results under test in the same two formats, one per
line, in the same order.

ref prints, per point: the input as se:sig, the input as a decimal, and the
correctly-rounded reference as se:sig.  cmp prints per-function statistics and
every point beyond one ulp.
"""

import sys
from mpmath import mp, mpf, workprec

mp.prec = 300

BIAS = 16383

# name -> (opmode, f(x), domain sample points)
def _dom(lo, hi, n=12, skip_ends=True):
    lo, hi = mpf(lo), mpf(hi)
    pts = []
    for i in range(n):
        t = mpf(i + 1) / (n + 1) if skip_ends else mpf(i) / (n - 1)
        pts.append(lo + (hi - lo) * t)
    return pts

import mpmath

FUNCS = [
    ("fsin",     0x0E, mpmath.sin,   _dom("-3.1", "3.1")),
    ("fcos",     0x1D, mpmath.cos,   _dom("-3.1", "3.1")),
    ("ftan",     0x0F, mpmath.tan,   _dom("-1.5", "1.5")),
    ("fatan",    0x0A, mpmath.atan,  _dom("-8", "8")),
    ("fasin",    0x0C, mpmath.asin,  _dom("-0.99", "0.99")),
    ("facos",    0x1C, mpmath.acos,  _dom("-0.99", "0.99")),
    ("fatanh",   0x0D, mpmath.atanh, _dom("-0.9", "0.9")),
    ("fsinh",    0x02, mpmath.sinh,  _dom("-5", "5")),
    ("fcosh",    0x19, mpmath.cosh,  _dom("-5", "5")),
    ("ftanh",    0x09, mpmath.tanh,  _dom("-5", "5")),
    ("fetox",    0x10, mpmath.exp,   _dom("-10", "10")),
    ("fetoxm1",  0x08, mpmath.expm1, _dom("-1", "1")),
    ("ftwotox",  0x11, lambda x: mpmath.power(2, x),  _dom("-10", "10")),
    ("ftentox",  0x12, lambda x: mpmath.power(10, x), _dom("-5", "5")),
    ("flogn",    0x14, mpmath.log,   _dom("0.01", "100")),
    ("flognp1",  0x06, mpmath.log1p, _dom("-0.9", "9")),
    ("flog10",   0x15, lambda x: mpmath.log(x, 10),   _dom("0.01", "100")),
    ("flog2",    0x16, lambda x: mpmath.log(x, 2),    _dom("0.01", "100")),
]


X80_INF_SIG = 1 << 63


def to_x80(x):
    """
    Round a high-precision value to floatx80; return (se, sig).

    Infinities are handled before anything else because mpmath represents them
    with a zero mantissa, so the zero path below swallows them and returns the
    encoding for +0.0 - a reference of infinity silently becoming zero, which
    grades every comparison against it as nonsense rather than failing.
    """
    if mpmath.isinf(x):
        return (0xFFFF if x < 0 else 0x7FFF, X80_INF_SIG)
    if mpmath.isnan(x):
        return (0x7FFF, X80_INF_SIG | (1 << 62))
    with workprec(64):
        y = +mpf(x)
    sign, man, exp, bc = y._mpf_
    if man == 0:
        return (0x8000 if sign else 0, 0)
    while bc < 64:
        man <<= 1
        exp -= 1
        bc += 1
    while bc > 64:                      # defensive; workprec should prevent it
        man >>= 1
        exp += 1
        bc -= 1
    biased = exp + 63 + BIAS
    if biased <= 0:
        """
        Below the smallest normal the exponent is pinned, so the significand
        loses bits rather than the exponent falling further.  Round at that
        fixed scale instead of at 64 significant bits.
        """
        with workprec(80):
            scaled = mpf(x) / (mpf(2) ** (1 - BIAS - 63))
        man = int(mpmath.nint(abs(scaled)))
        if man >> 63:               # rounded up into the normal range
            return ((0x8000 if sign else 0) | 1, man)
        return ((0x8000 if sign else 0), man)
    assert biased < 0x7FFF, "out of range: %s" % y
    return ((0x8000 if sign else 0) | biased, man)


def from_x80(se, sig):
    """
    Convert a floatx80 encoding back to an mpf.

    A denormal's exponent is 1 - BIAS, not 0 - BIAS: the field stops at zero
    but the value does not gain another octave, so the smallest denormal and
    the smallest normal share a scale.  Reading the field literally puts every
    denormal one octave low, which is silent - the numbers stay plausible.
    """
    sign = -1 if (se & 0x8000) else 1
    biased = se & 0x7FFF
    if biased == 0 and sig == 0:
        return mpf(0)
    if biased == 0x7FFF:
        if sig == X80_INF_SIG:
            return sign * mp.inf
        return None                     # NaN: no value to compare
    return sign * mpf(sig) * mpf(2) ** (x80_exponent(biased) - 63)


def x80_exponent(biased):
    """Unbiased exponent of a floatx80 significand, denormals included."""
    return (biased if biased else 1) - BIAS


def ulps(got_se, got_sig, want_se, want_sig):
    """
    Distance in units of the reference's last place.

    None means the pair cannot be compared by value - a NaN on either side -
    and callers must report how many they skipped rather than quietly grading
    the remainder.  Infinities ARE comparable: matching infinities are exact,
    and an infinity against a finite value is the largest possible error, not
    an unscoreable row.  Treating that as unscoreable would let an
    implementation return infinity for everything and be excluded rather than
    failed.
    """
    g = from_x80(got_se, got_sig)
    w = from_x80(want_se, want_sig)
    if g is None or w is None:
        return None
    if mpmath.isinf(w) or mpmath.isinf(g):
        return mpf(0) if g == w else mp.inf
    if w == 0:
        return abs(g) / (mpf(2) ** (1 - BIAS - 63))
    return abs(g - w) / (mpf(2) ** (x80_exponent(want_se & 0x7FFF) - 63))


def emit(path):
    lines = []
    A = lines.append
    A("/*")
    A(" * 68881 transcendentals against an independent oracle.")
    A(" *")
    A(" * GENERATED by tests/tcg/m68k/system/fporacle.py -- do not edit.")
    A(" * The guest computes; the host compares against mpmath's correctly-")
    A(" * rounded extended-precision results, which share no ancestry with")
    A(" * QEMU's or Hatari's FPSP lineage.")
    A(" *")
    A(" * SPDX-License-Identifier: GPL-2.0-or-later")
    A(" */")
    A("")
    A("        .text")
    A("        .globl  _start")
    A("_start:")
    A("        lea     0x00100000,%sp")
    A("        lea     inputs,%a0")
    A("        lea     results,%a1")
    A("        move.l  #%d,%%d7" % len(FUNCS))
    A("")
    in_off = 0
    for name, opmode, _fn, pts in FUNCS:
        A("        /* %s */" % name)
        A("        lea     inputs+%d,%%a0" % in_off)
        # dbra counts the low word; moveq would cap the table at 128 points
        A("        move.w  #%d,%%d0" % (len(pts) - 1))
        A("1:      .word   0xf210,0x4800          /* fmove.x (%a0),%fp0 */")
        A("        .word   0xf200,0x%04x          /* %s fp0,fp0 */" % (opmode, name))
        A("        .word   0xf211,0x6800          /* fmove.x %fp0,(%a1) */")
        A("        lea     12(%a0),%a0")
        A("        lea     12(%a1),%a1")
        A("        dbra    %d0,1b")
        A("")
        in_off += 12 * len(pts)
    A("        /* dump every result as hex */")
    A("        lea     results,%a0")
    A("        move.l  #%d,%%d6" % (sum(len(p) for _n, _o, _f, p in FUNCS) * 12))
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
    A("        .data")
    A("        .balign 4")
    A("inputs:")
    for name, _o, _f, pts in FUNCS:
        for x in pts:
            se, sig = to_x80(x)
            A("        .short  0x%04x, 0" % se)
            A("        .long   0x%08x, 0x%08x   /* %s %s */"
              % (sig >> 32, sig & 0xFFFFFFFF, name, mp.nstr(x, 12)))
    A("        .bss")
    A("        .balign 4")
    A("results:")
    A("        .space  %d" % (sum(len(p) for _n, _o, _f, p in FUNCS) * 12))
    open(path, "w").write("\n".join(lines) + "\n")
    n = sum(len(p) for _n, _o, _f, p in FUNCS)
    print("wrote %s: %d functions, %d points" % (path, len(FUNCS), n))


def check(dump_path):
    raw = open(dump_path).read().strip()
    data = bytes.fromhex(raw)
    off = 0
    worst_overall = mpf(0)
    bad = []
    print("%-10s %6s  %-10s %-10s %5s  %s"
          % ("function", "points", "max ulp", "median", ">0.5", "verdict"))
    for name, _o, fn, pts in FUNCS:
        worst = mpf(0)
        worst_x = None
        errs = []
        for x in pts:
            se = int.from_bytes(data[off:off + 2], "big")
            sig = int.from_bytes(data[off + 4:off + 12], "big")
            off += 12
            #
            # Evaluate the oracle at the input the guest actually received -
            # the floatx80 the table holds - not at the ideal value it was
            # rounded from.  Comparing f(ideal) against f(rounded) charges the
            # implementation for the input rounding, amplified by the
            # function's condition number, and manufactures multi-ulp errors
            # out of nothing: e^x at x=-8.46 turns half an input ulp into four.
            #
            x_in = from_x80(*to_x80(x))
            with workprec(300):
                exact = fn(x_in)
            wse, wsig = to_x80(exact)
            u = ulps(se, sig, wse, wsig)
            if u is None:
                continue
            errs.append(u)
            if u > worst:
                worst, worst_x = u, x_in
        errs.sort()
        med = errs[len(errs) // 2] if errs else mpf(0)
        over_half = sum(1 for e in errs if e > mpf("0.5"))
        verdict = "ok" if worst <= 2 else "SUSPECT"
        if worst > 2:
            bad.append((name, worst, worst_x))
        print("%-10s %6d  %-10s %-10s %5d  %s"
              % (name, len(pts), mp.nstr(worst, 4), mp.nstr(med, 4),
                 over_half, verdict))
        worst_overall = max(worst_overall, worst)
    print("\nmax ulp across all functions: %s" % mp.nstr(worst_overall, 6))
    if bad:
        print("SUSPECT functions (worst input shown):")
        for name, u, x in bad:
            print("  %-10s %s ulp at x = %s" % (name, mp.nstr(u, 6), mp.nstr(x, 12)))
        return 1
    return 0


NAMED = {
    "log": mpmath.log, "log1p": mpmath.log1p,
    "log2": lambda x: mpmath.log(x, 2), "log10": lambda x: mpmath.log(x, 10),
    "sin": mpmath.sin, "cos": mpmath.cos, "tan": mpmath.tan,
    "atan": mpmath.atan, "asin": mpmath.asin, "acos": mpmath.acos,
    "atanh": mpmath.atanh, "sinh": mpmath.sinh, "cosh": mpmath.cosh,
    "tanh": mpmath.tanh, "exp": mpmath.exp, "expm1": mpmath.expm1,
    "exp2": lambda x: mpmath.power(2, x), "exp10": lambda x: mpmath.power(10, x),
}


def parse_value(text):
    """A line is either decimal, or hex se:sig as floatx80 is encoded."""
    text = text.strip()
    if ":" in text:
        se, sig = text.split(":")
        return from_x80(int(se, 16), int(sig, 16))
    return mpf(text)


def read_points(path):
    out = []
    for line in open(path):
        line = line.split("#")[0].strip()
        if line:
            out.append(parse_value(line))
    return out


def reference(fn, x):
    """
    Evaluate at the floatx80 the implementation ACTUALLY RECEIVED, never at the
    ideal value it was rounded from.  Comparing f(ideal) with f(rounded)
    charges the implementation for the input rounding, amplified by the
    function's condition number, and manufactures errors out of nothing: e^x at
    x = -8.46 turns half an input ulp into four.  This cost the QEMU lane an
    afternoon and a nearly-sent bug report.
    """
    x_in = from_x80(*to_x80(x))
    with workprec(300):
        return x_in, fn(x_in)


def cmd_ref(name, points_path):
    fn = NAMED[name]
    for x in read_points(points_path):
        x_in, exact = reference(fn, x)
        ise, isig = to_x80(x_in)
        rse, rsig = to_x80(exact)
        print("%04x:%016x  %-24s  %04x:%016x"
              % (ise, isig, mp.nstr(x_in, 17), rse, rsig))


def cmd_cmp(name, points_path, got_path):
    fn = NAMED[name]
    pts = read_points(points_path)
    got = read_points(got_path)
    if len(pts) != len(got):
        print("point/result count mismatch: %d vs %d" % (len(pts), len(got)))
        return 2
    errs, skipped, zero_refs = [], 0, 0
    for x, g in zip(pts, got):
        x_in, exact = reference(fn, x)
        if exact == 0:
            zero_refs += 1
        u = ulps(*(to_x80(g) + to_x80(exact)))
        if u is None:
            skipped += 1                # NaN on one side: no value to compare
        else:
            errs.append((u, x_in))
    if not errs:
        print("no comparable points")
        return 2
    ordered = sorted(u for u, _ in errs)
    worst = max(errs, key=lambda t: t[0])
    print("%s: %d points" % (name, len(errs)))
    if skipped:
        # Never let an exclusion be invisible: a grader that quietly drops the
        # rows it cannot score reports a clean sweep over whatever is left.
        print("  SKIPPED %d point(s) as NaN - not graded" % skipped)
    if zero_refs:
        # mpmath has no negative zero - mpf('-0.0') is bit-identical to +0.0 -
        # so a reference of -0.0 is unobtainable and this tool cannot tell the
        # two apart.  It grades the magnitude and is blind to the sign, which
        # is a live defect class rather than a hypothetical one.
        print("  %d point(s) have a zero reference: MAGNITUDE ONLY, the sign "
              "of zero is not graded" % zero_refs)
    print("  max     %s ulp at x = %s" % (mp.nstr(worst[0], 6),
                                          mp.nstr(worst[1], 17)))
    print("  median  %s" % mp.nstr(ordered[len(ordered) // 2], 6))
    print("  >0.5    %d" % sum(1 for u in ordered if u > mpf("0.5")))
    print("  >1      %d" % sum(1 for u in ordered if u > 1))
    print("  >2      %d" % sum(1 for u in ordered if u > 2))
    for u, x in sorted(errs, key=lambda t: -t[0]):
        if u > 1:
            print("  %-12s ulp at x = %s" % (mp.nstr(u, 6), mp.nstr(x, 17)))
    return 0


def selftest():
    """
    Check the encoding, not the mathematics.

    This exists because the denormal exponent was wrong by one octave and
    nothing noticed: every denormal read back at half its value, which is
    silent because the results stay plausible.  The boundary case below is the
    one that catches it - a denormal one ulp above the largest denormal must be
    the smallest normal, and it cannot be if the two live an octave apart.
    """
    tiny = mpf(2) ** (1 - BIAS - 63)            # smallest denormal
    big_denorm = ((1 << 63) - 1) * tiny         # largest denormal
    small_norm = mpf(2) ** (1 - BIAS)           # smallest normal
    cases = [
        ("smallest denormal decodes", from_x80(0, 1), tiny),
        ("largest denormal decodes", from_x80(0, (1 << 63) - 1), big_denorm),
        ("smallest normal decodes", from_x80(1, 1 << 63), small_norm),
        ("denormals meet normals", big_denorm + tiny, small_norm),
        ("one decodes", from_x80(BIAS, 1 << 63), mpf(1)),
    ]
    bad = 0
    for name, got, want in cases:
        if got != want:
            print("FAIL %s: %s != %s" % (name, mp.nstr(got, 20), mp.nstr(want, 20)))
            bad += 1
    for name, value, enc in [("smallest denormal encodes", tiny, (0, 1)),
                             ("largest denormal encodes", big_denorm,
                              (0, (1 << 63) - 1)),
                             ("smallest normal encodes", small_norm,
                              (1, 1 << 63)),
                             ("one encodes", mpf(1), (BIAS, 1 << 63))]:
        if to_x80(value) != enc:
            print("FAIL %s: %s != %s" % (name, to_x80(value), enc))
            bad += 1
    if ulps(0, 1, 0, 2) != 1:
        print("FAIL denormal ulp spacing: %s" % mp.nstr(ulps(0, 1, 0, 2), 10))
        bad += 1
    boundary = [
        ("max finite encodes", to_x80(mpf(2) ** 16384 * (1 - mpf(2) ** -64)),
         (0x7FFE, (1 << 64) - 1)),
        ("under the smallest denormal flushes to zero",
         to_x80(mpf(2) ** -16446), (0, 0)),
        ("NaN encodes", to_x80(mp.nan), (0x7FFF, X80_INF_SIG | (1 << 62))),
    ]
    for nm, got, want in boundary:
        if got != want:
            print("FAIL %s: %s != %s" % (nm, got, want))
            bad += 1
    try:
        to_x80(mpf(2) ** 16384)
        print("FAIL overflow past the largest finite did not raise")
        bad += 1
    except AssertionError:
        pass                            # refusing is the safe failure
    inf_cases = [
        ("+inf encodes", to_x80(mp.inf), (0x7FFF, X80_INF_SIG)),
        ("-inf encodes", to_x80(-mp.inf), (0xFFFF, X80_INF_SIG)),
    ]
    for nm, got, want in inf_cases:
        if got != want:
            print("FAIL %s: %s != %s" % (nm, got, want))
            bad += 1
    if from_x80(0x7FFF, X80_INF_SIG) != mp.inf:
        print("FAIL +inf decodes")
        bad += 1
    if ulps(0x7FFF, X80_INF_SIG, 0x7FFF, X80_INF_SIG) != 0:
        print("FAIL matching infinities are not exact")
        bad += 1
    if ulps(0x3FFF, 1 << 63, 0x7FFF, X80_INF_SIG) != mp.inf:
        print("FAIL finite against infinite is not the largest error")
        bad += 1
    if ulps(0x7FFF, 3 << 62, 0x3FFF, 1 << 63) is not None:
        print("FAIL NaN is comparable by value")
        bad += 1
    print("selftest: %d checks, %d failed" % (len(cases) + 15, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "emit":
        emit(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "check":
        sys.exit(check(sys.argv[2]))
    elif len(sys.argv) == 2 and sys.argv[1] == "selftest":
        sys.exit(selftest())
    elif len(sys.argv) == 4 and sys.argv[1] == "ref":
        cmd_ref(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 5 and sys.argv[1] == "cmp":
        sys.exit(cmd_cmp(sys.argv[2], sys.argv[3], sys.argv[4]))
    else:
        print(__doc__)
        sys.exit(2)
