#!/usr/bin/env python3
# Judges results against high-precision arithmetic (mpmath).
#   verify.py FILE            FILE holds "name arguments results" lines (the output of "check eval");
#                             prints, per function, the largest error in units in the last place and where it is
#   verify.py --refine FILE   prints the lines with the results replaced by the correctly rounded ones
#                             (this is how expected.txt is freed from the errors of the host's library)
# Lines whose result is not an ordinary number (NaN, infinite arguments, domain errors) are left alone.
import sys, struct, math, re, os
from fractions import Fraction
from mpmath import mp, mpf
import mpmath

refine = "--refine" in sys.argv
path = [a for a in sys.argv[1:] if not a.startswith("--")][0]

def u2d(u): return struct.unpack("<d", struct.pack("<Q", u))[0]
def u2f(u): return struct.unpack("<f", struct.pack("<I", u & 0xffffffff))[0]
def d2u(x): return struct.unpack("<Q", struct.pack("<d", x))[0]
def f2u(x): return struct.unpack("<I", struct.pack("<f", x))[0]

FMT = {True: (53, -1074, 1024), False: (24, -149, 128)}
def ulp_of(v, dbl):
    p, emin, emax = FMT[dbl]
    if v == 0: return mpf(2) ** emin
    e = int(mpmath.floor(mpmath.log(abs(v), 2)))
    if mpf(2) ** e > abs(v): e -= 1
    if mpf(2) ** (e + 1) <= abs(v): e += 1
    return mpf(2) ** max(min(e, emax - 1) - p + 1, emin)
def round_to(v, dbl):
    "correctly rounded value as a python float (None when it would be zero: the sign is the caller's business)"
    p, emin, emax = FMT[dbl]
    if v == 0: return None
    if abs(v) >= mpf(2) ** emax: return math.copysign(math.inf, float(mpmath.sign(v)))
    u = ulp_of(v, dbl)
    m = int(mpmath.nint(v / u))
    r = mpf(m) * u
    if abs(r) >= mpf(2) ** emax: return math.copysign(math.inf, float(mpmath.sign(v)))
    if r == 0: return None
    return float(r)

def lgam(x):
    if x > 0: return mpmath.loggamma(x)
    return mpmath.log(abs(mpmath.gamma(x)))
def powf(x, y):
    if x < 0:
        if y != mpmath.floor(y): raise ValueError
        r = mpmath.power(-x, y)
        return -r if int(y) % 2 else r
    return mpmath.power(x, y)
def frac_fmod(x, y):
    fx, fy = Fraction(float(x)), Fraction(float(y))
    q = abs(fx) // abs(fy)
    r = abs(fx) - q * abs(fy)
    return mpf(r.numerator) / r.denominator * (1 if x >= 0 else -1)
def frac_rem(x, y):
    fx, fy = Fraction(float(x)), Fraction(float(y))
    q = fx / fy
    n = math.floor(q)
    d = q - n
    if d > Fraction(1, 2) or (d == Fraction(1, 2) and n % 2): n += 1
    r = fx - n * fy
    return mpf(r.numerator) / r.denominator
F1 = {
 "sin": mpmath.sin, "cos": mpmath.cos, "tan": mpmath.tan, "asin": mpmath.asin, "acos": mpmath.acos, "atan": mpmath.atan,
 "sinh": mpmath.sinh, "cosh": mpmath.cosh, "tanh": mpmath.tanh, "asinh": mpmath.asinh, "acosh": mpmath.acosh, "atanh": mpmath.atanh,
 "exp": mpmath.exp, "exp2": lambda x: mpmath.power(2, x), "exp10": lambda x: mpmath.power(10, x), "pow10": lambda x: mpmath.power(10, x),
 "expm1": mpmath.expm1, "log": mpmath.log, "log2": lambda x: mpmath.log(x, 2), "log10": mpmath.log10, "log1p": mpmath.log1p,
 "cbrt": lambda x: mpmath.cbrt(x) if x >= 0 else -mpmath.cbrt(-x), "sqrt": mpmath.sqrt,
 "erf": mpmath.erf, "erfc": mpmath.erfc, "tgamma": mpmath.gamma, "lgamma": lgam, "gamma": lgam, "lgamma_r": lgam,
 "j0": mpmath.j0, "j1": mpmath.j1, "y0": lambda x: mpmath.bessely(0, x), "y1": lambda x: mpmath.bessely(1, x),
}
F2 = {"atan2": mpmath.atan2, "pow": powf, "hypot": mpmath.hypot, "fmod": frac_fmod, "remainder": frac_rem, "drem": frac_rem}
kinds = {}
here = os.path.dirname(os.path.abspath(__file__))
for line in open(os.path.join(here, "funcs.h")):
    m = re.match(r"\s*[EM]\((\w+), (\w+)", line)
    if m: kinds[m.group(2)] = m.group(1)

stats = {}
def base_of(name, isf):
    if isf and name.endswith("f") and name[:-1] in list(F1) + list(F2) + ["sincos", "fma", "scalbn", "ldexp", "scalbln"]: return name[:-1]
    if name == "lgammaf_r": return "lgamma_r"
    if not isf and name.endswith("l") and name[:-1] in list(F1) + list(F2) + ["fma", "scalbn", "ldexp"]: return name[:-1]
    return name

def exact(base, args, dbl):
    "list of exact results (mpf), or None"
    if any(a != a or abs(a) == math.inf for a in args if isinstance(a, float)): return None
    big = max([abs(a) for a in args if isinstance(a, float)] + [1.0])
    mp.prec = 400
    if base in ("sin", "cos", "tan", "sincos"):
        mp.prec = 400 + max(0, math.frexp(big)[1]) * 2
    x = [mpf(a) if isinstance(a, float) else a for a in args]
    try:
        if base == "sincos": return [mpmath.sin(x[0]), mpmath.cos(x[0])]
        if base == "fma":
            r = Fraction(args[0]) * Fraction(args[1]) + Fraction(args[2])
            mp.prec = 5000
            return [mpf(r.numerator) / r.denominator]
        if base in ("scalbn", "ldexp", "scalbln"):
            mp.prec = 3000
            return [x[0] * mpf(2) ** max(min(args[1], 5000), -5000)]
        if base in F1: r = F1[base](x[0])
        elif base in F2:
            if base == "atan2" and (args[0] == 0 or args[1] == 0): return None  # signed zeros
            if base in ("fmod", "remainder", "drem"):
                if args[1] == 0: return None
                mp.prec = 3000
            r = F2[base](x[0], x[1])
        else: return None
    except (ValueError, ZeroDivisionError, OverflowError, TypeError):
        return None
    if isinstance(r, mpmath.mpc) or r != r or abs(r) == mpmath.inf: return None
    return [r]

for line in open(path):
    w = line.split()
    if not w or w[0] not in kinds:
        if refine: sys.stdout.write(line)
        continue
    name, kind = w[0], kinds[w[0]]
    isf = kind.startswith("F") or kind in ("L_F", "L_FF", "SC_F")
    conv = u2f if isf else u2d
    base = base_of(name, isf)
    nin = 3 if kind in ("D_DDD", "F_FFF") else 2 if kind in ("D_DD", "F_FF", "L_DD", "L_FF", "D_DI", "F_FI", "D_DL", "F_FL", "D_DD_PI", "F_FF_PI") else 1
    vals = [int(v, 16) for v in w[1:]]
    ins = vals[:nin]; outs = vals[nin:]
    if kind in ("D_DI", "F_FI", "D_DL", "F_FL"):
        n = ins[1] - (1 << 64) if ins[1] >> 63 else ins[1]
        if kind in ("D_DI", "F_FI"): n = (n + 2**31) % 2**32 - 2**31
        args = [conv(ins[0]), n]
    else:
        args = [conv(v) for v in ins]
    ex = None
    if kind[0] in "DFS" and (base in F1 or base in F2 or base in ("sincos", "fma", "scalbn", "ldexp", "scalbln")):
        ex = exact(base, args, not isf)
    if ex is None:
        if refine: sys.stdout.write(line)
        continue
    new = list(outs)
    for i, e in enumerate(ex):
        got = conv(outs[i])
        if got != got or abs(got) == math.inf:
            r = round_to(e, not isf)
            err = mpf(0) if r is not None and abs(r) == math.inf and r == got else mpf(10) ** 6
        else:
            err = abs(mpf(got) - e) / ulp_of(e, not isf)
        if refine:
            r = round_to(e, not isf)
            if r is not None: new[i] = (f2u if isf else d2u)(r)
        if "--each" in sys.argv: print(name, args, got, mpmath.nstr(err, 4))
        st = stats.setdefault(name, [0, mpf(0), None])
        st[0] += 1
        if err > st[1]: st[1] = err; st[2] = " ".join(w[1:1 + nin])
    if refine:
        print(name, " ".join("%x" % v for v in ins + new))
if not refine:
    for name, (n, err, where) in stats.items():
        print("%-10s %6d cases, max error %s ulp%s" % (name, n, mpmath.nstr(err, 4), " (at %s)" % where if where else ""))
