#!/usr/bin/env python3
# Prints test inputs for tests/ext/libm/check.c: "name hexbits..." lines.
# usage: gen.py N [SEED] [name...]     about N inputs per function (some kinds get fewer)
# The expected values are then made by the host:  cc -DHOSTGEN check.c -o hostcheck; gen.py 40 | ./hostcheck eval > expected.txt
import sys, struct, random, math, re, os

N = int(sys.argv[1]) if len(sys.argv) > 1 else 40
rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
only = set(sys.argv[3:])

def d2u(x): return struct.unpack("<Q", struct.pack("<d", x))[0]
def u2d(u): return struct.unpack("<d", struct.pack("<Q", u))[0]
def f2u(x):
    try: return struct.unpack("<I", struct.pack("<f", x))[0]
    except OverflowError: return 0x7f800000 if x > 0 else 0xff800000
def u2f(u): return struct.unpack("<f", struct.pack("<I", u))[0]
inf = float("inf"); nan = float("nan")

class T:  # one floating-point format
    def __init__(s, dbl):
        s.dbl = dbl
        if dbl: s.bits, s.unbits, s.ebits, s.mbits, s.emax = d2u, u2d, 11, 52, 1023
        else: s.bits, s.unbits, s.ebits, s.mbits, s.emax = f2u, u2f, 8, 23, 127
        s.big = s.unbits(((1 << s.ebits) - 2) << s.mbits | ((1 << s.mbits) - 1))
        s.tiny = s.unbits(1); s.minnorm = s.unbits(1 << s.mbits)
        s.eps = 2.0 ** -s.mbits
    def rt(s, x): return s.unbits(s.bits(x))  # round to the format
    def rbits(s):
        return s.unbits(rnd.getrandbits(s.ebits + s.mbits + 1))
    def rfinite(s):
        while True:
            x = s.rbits()
            if x == x and abs(x) != inf: return x
    def rexp(s, lo, hi):  # random with binary exponent in [lo, hi]
        if not s.dbl: lo = lo * 127 // 1023; hi = hi * 127 // 1023  # the ranges are given for double
        lo = min(max(lo, -s.emax - s.mbits + 1), s.emax); hi = max(min(hi, s.emax), lo)
        e = rnd.randint(lo, hi)
        return s.rt(math.ldexp(1 + rnd.random(), e) * rnd.choice((1, -1)))
    def uni(s, a, b): return s.rt(rnd.uniform(a, b))
    def logu(s, a, b, signed=True):
        a = max(a, s.tiny); b = min(b, s.big)
        x = math.exp(rnd.uniform(math.log(a), math.log(b)))
        return s.rt(x * (rnd.choice((1, -1)) if signed else 1))
    def near(s, x, k=4):  # a few ulps around x
        u = s.bits(s.rt(x)); u = max(0, u + rnd.randint(-k, k))
        return s.unbits(u)
    def specials(s):
        return [0.0, -0.0, inf, -inf, nan, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 1.5, 2.5, -2.5, 3.0, 10.0, s.tiny, -s.tiny, s.minnorm, -s.minnorm,
                s.unbits((1 << s.mbits) - 1), s.big, -s.big, 1 + s.eps, 1 - s.eps / 2, s.rt(math.pi), s.rt(math.pi / 2), s.rt(math.e), 1e-5, 0.1, 100.0, s.rt(1e10), s.rt(-1e10)]

def mix(t, n, gens):
    "n inputs from weighted generators"
    out = []
    tot = sum(w for w, g in gens)
    for w, g in gens:
        for i in range(max(1, int(round(n * w / tot)))):
            out.append(g())
    return out

def ranges1(t, f):
    R = t.rfinite
    L = lambda a, b, sg=True: (lambda: t.logu(a, b, sg))
    U = lambda a, b: (lambda: t.uni(a, b))
    big = t.big
    g = {
     "sin": [(3, U(-10, 10)), (2, L(1e-9, 1e6)), (2, L(1e6, big)), (1, R), (1, lambda: t.near(rnd.randint(1, 1000) * math.pi / 2, 3)), (1, U(-0.8, 0.8))],
     "asin": [(4, U(-1, 1)), (1, L(1e-9, 1)), (1, lambda: t.near(rnd.choice((1, -1, 0.5, -0.5)), 30)), (0.3, R)],
     "atan": [(4, L(1e-9, 1e9)), (2, U(-3, 3)), (1, R)],
     "sinh": [(3, U(-25, 25)), (2, L(1e-9, 720 if t.dbl else 90)), (1, U(-1, 1)), (0.5, R)],
     "asinh": [(4, L(1e-9, 1e9)), (2, U(-3, 3)), (1, R)],
     "acosh": [(3, lambda: t.rt(1 + abs(t.logu(1e-16, 1e3)))), (2, L(1, big, False)), (0.3, R)],
     "atanh": [(4, U(-1, 1)), (1, L(1e-9, 1)), (1, lambda: t.near(rnd.choice((1, -1)), 30)), (0.3, R)],
     "exp": [(3, U(-745, 710) if t.dbl else U(-104, 89)), (3, U(-5, 5)), (2, L(1e-20, 1)), (0.5, R)],
     "exp2": [(3, U(-1075, 1024) if t.dbl else U(-150, 128)), (3, U(-10, 10)), (1, L(1e-20, 1)), (0.5, R), (0.5, lambda: float(rnd.randint(-60, 60)))],
     "exp10": [(3, U(-324, 309) if t.dbl else U(-46, 39)), (3, U(-5, 5)), (1, L(1e-20, 1)), (0.5, R), (0.5, lambda: float(rnd.randint(-25, 25)))],
     "expm1": [(3, U(-40, 710) if t.dbl else U(-20, 89)), (3, U(-2, 2)), (2, L(1e-20, 1)), (0.5, R)],
     "log": [(3, lambda: abs(R())), (3, U(0.5, 2)), (2, lambda: t.rt(1 + t.logu(1e-16, 0.2))), (2, L(1e-6, 1e6, False)), (0.5, lambda: abs(t.rexp(-1080, -t.emax))), (0.3, R)],
     "log1p": [(3, L(1e-20, 1)), (3, U(-1, 10)), (2, L(1, big, False)), (1, lambda: t.near(-1, 30)), (0.3, R)],
     "cbrt": [(3, R), (2, U(-100, 100)), (1, L(1e-6, 1e6)), (0.5, lambda: t.rexp(-1080, -t.emax))],
     "erf": [(4, U(-6, 6)), (2, L(1e-20, 1)), (0.5, R)],
     "erfc": [(4, U(-6, 28) if t.dbl else U(-4, 11)), (2, L(1e-20, 1)), (0.5, R)],
     "tgamma": [(4, U(-180, 172) if t.dbl else U(-40, 36)), (2, U(0, 10)), (1, L(1e-20, 1)), (0.5, lambda: float(rnd.randint(-30, 40))), (0.3, R)],
     "lgamma": [(3, U(-100, 100)), (2, U(0, 10)), (2, L(1e-20, big)), (1, lambda: t.rt(rnd.choice((1, 2)) + t.logu(1e-10, 0.3))), (0.5, lambda: float(rnd.randint(-30, 40))), (0.3, R)],
     "round": [(3, U(-100, 100)), (2, lambda: rnd.randint(-1000, 1000) + 0.5), (2, R), (1, lambda: t.near(2.0 ** rnd.randint(t.mbits - 2, t.mbits + 2), 5) * rnd.choice((1, -1))), (1, L(1e-3, 1e18)), (1, lambda: t.near(rnd.choice((0.5, -0.5, 1.5)), 2))],
     "any": [(1, R)],
     "sqrt": [(3, lambda: abs(R())), (2, U(0, 100)), (0.5, R), (0.5, lambda: abs(t.rexp(-1080, -t.emax)))],
    }
    alias = {"cos": "sin", "tan": "sin", "sincos": "sin", "acos": "asin", "cosh": "sinh", "tanh": "sinh", "log2": "log", "log10": "log", "pow10": "exp10",
             "gamma": "lgamma", "lgamma_r": "lgamma"}
    for r in "floor ceil trunc rint nearbyint lrint lround llrint llround modf".split(): alias[r] = "round"
    return g.get(alias.get(f, f), g["any"])

def gen1(t, f, n):
    xs = mix(t, n, ranges1(t, f))
    return [(x,) for x in t.specials() + xs]

def pairs(t, f, n):
    R = t.rfinite; S = t.specials(); out = []
    grid = [0.0, -0.0, inf, -inf, nan, 1.0, -1.0, 0.5, 2.0, -2.0, 3.0, -3.0, t.tiny, -t.big, t.big, 1.5, -0.5]
    out += [(a, b) for a in grid for b in grid]
    k = lambda: rnd.randint(0, 3)
    if f == "pow":
        g = [(3, lambda: (abs(t.logu(1e-5, 1e5)), t.uni(-20, 20))),
             (2, lambda: (t.rt(1 + t.logu(1e-12, 0.01)), t.logu(1, 1e12))),
             (3, lambda: (lambda x: (x, t.rt(rnd.uniform(-(t.emax + t.mbits), t.emax + 1) * math.log(2) / (math.log(x) if x != 1 else 1))))(abs(R()) or 1.5)),
             (1, lambda: (-abs(t.logu(1e-3, 1e3)), float(rnd.randint(-40, 40)))),
             (1, lambda: (float(rnd.randint(1, 30)), float(rnd.randint(-15, 30)))),
             (1, lambda: (abs(t.uni(0, 4)), t.uni(-4, 4))),
             (1, lambda: (R(), R()))]
    elif f in ("atan2", "hypot"):
        g = [(3, lambda: (t.logu(1e-6, 1e6), t.logu(1e-6, 1e6))), (2, lambda: (t.uni(-10, 10), t.uni(-10, 10))), (2, lambda: (R(), R())),
             (1, lambda: (lambda x: (x, t.rt(x * rnd.uniform(-2, 2))))(R())), (1, lambda: (t.rexp(-1080, -t.emax + 5), t.rexp(-1080, -t.emax + 5)))]
    elif f in ("fmod", "remainder", "remquo", "drem"):
        g = [(2, lambda: (R(), R())), (3, lambda: (t.uni(-100, 100), t.uni(-10, 10))),
             (2, lambda: (lambda y: (t.rt(y * (rnd.randint(-9, 9) + rnd.choice((0, 0.5, 0.25, rnd.random())))), y))(t.logu(1e-3, 1e3))),
             (2, lambda: (lambda y: (t.rt(y * rnd.uniform(-2 ** 30, 2 ** 30)), y))(R())),
             (1, lambda: (t.rexp(-1080, -t.emax + 5), t.rexp(-1080, -t.emax + 5))), (1, lambda: (t.rexp(0, 1100), t.rexp(-1080, 0))),
             (1, lambda: (float(rnd.randint(-50, 50)), float(rnd.randint(1, 9))))]
    elif f in ("nextafter", "nexttoward"):
        g = [(2, lambda: (R(), R())), (2, lambda: (lambda x: (x, rnd.choice((x, inf, -inf, 0.0, -x, t.near(x, 2)))))(R())),
             (1, lambda: (rnd.choice(S), rnd.choice(S)))]
    else:
        g = [(2, lambda: (R(), R())), (2, lambda: (t.uni(-10, 10), t.uni(-10, 10))), (1, lambda: (lambda x: (x, rnd.choice((x, -x, t.near(x, 2)))))(R())),
             (1, lambda: (rnd.choice(S), rnd.choice(S)))]
    return out + mix(t, n, g)

def triples(t, n):
    R = t.rfinite; S = t.specials(); out = []
    grid = [0.0, -0.0, inf, -inf, nan, 1.0, -1.5, t.tiny, t.big]
    out += [(a, b, c) for a in grid for b in grid for c in grid if rnd.random() < 0.35]
    def cancel():
        a = t.uni(-100, 100); b = t.uni(-100, 100)
        return (a, b, t.near(-a * b, 3))
    def cancel2():
        a = t.rexp(-60, 60); b = t.rexp(-60, 60)
        return (a, b, t.near(-a * b, 2))
    g = [(3, lambda: (t.uni(-10, 10), t.uni(-10, 10), t.uni(-10, 10))), (3, cancel), (2, cancel2), (2, lambda: (R(), R(), R())),
         (2, lambda: (t.rexp(-600, -400), t.rexp(-700, -400), t.rexp(-1080, -900))), (1, lambda: (t.rexp(400, 600), t.rexp(400, 600), t.rexp(900, 1023)))]
    return out + mix(t, n, g)

def scal(t, n):
    out = []
    for i in range(n):
        x = rnd.choice((t.rfinite, lambda: t.uni(-4, 4), lambda: rnd.choice(t.specials())))()
        m = rnd.choice((rnd.randint(-2300, 2300), rnd.randint(-60, 60), rnd.randint(-400, 400)))
        if x and abs(x) != inf and x == x and rnd.random() < 0.4:  # land near the subnormal boundary
            m = -t.emax - rnd.randint(-3, t.mbits + 3) - math.frexp(x)[1]
        out.append((x, m))
    return out

dbl = T(True); flt = T(False)
kinds = {}
here = os.path.dirname(os.path.abspath(__file__))
for line in open(os.path.join(here, "funcs.h")):
    m = re.match(r"\s*[EM]\((\w+), (\w+)", line)
    if m: kinds[m.group(2)] = m.group(1)
few = set("fabs copysign fdim fmin fmax logb ilogb fpclassify isnan isinf isfinite isnormal signbit finite isless nexttoward drem gamma pow10 scalbln ldexp".split())
for name, kind in kinds.items():
    if only and name not in only: continue
    isf = kind.startswith("F") or kind in ("L_F", "L_FF", "SC_F")
    t = flt if isf else dbl
    base = name
    if isf and base.endswith("f") and base not in ("erf",): base = base[:-1]
    if base == "lgammaf_r": base = "lgamma_r"
    if base == "er": base = "erf"
    n = N // 3 if base in few else N
    ld = name.endswith("l") and name[:-1] in kinds  # a long double name: a few cases will do
    if ld: base = name[:-1]; n = 6
    if kind in ("D_DD", "F_FF", "L_DD", "L_FF", "D_DD_PI", "F_FF_PI"):
        rows = pairs(t, base, n)
        if n < 100: rows = rnd.sample(rows[:289], min(289, max(20, n // 2))) + rows[289:]
    elif kind in ("D_DDD", "F_FFF"):
        rows = triples(t, n)
        if n < 100: rows = rnd.sample(rows, min(len(rows), n + 30))
    elif kind in ("D_DI", "F_FI", "D_DL", "F_FL"):
        rows = scal(t, n)
    else:
        rows = gen1(t, base, n)
        if kind in ("L_D", "L_F") and "r" in base[:3]:  # conversions to long are undefined out of range
            rows = [r for r in rows if abs(r[0]) < 2.0 ** 62]
    if ld: rows = rnd.sample(rows, min(len(rows), 12))
    for r in rows:
        if kind in ("D_DI", "F_FI", "D_DL", "F_FL"):
            print(name, "%x %x" % (t.bits(r[0]), r[1] & 0xffffffffffffffff))
        else:
            print(name, " ".join("%x" % t.bits(v) for v in r))
