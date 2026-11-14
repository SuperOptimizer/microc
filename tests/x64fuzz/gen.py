#!/usr/bin/env python3
# gen.py SEED: a random C program without undefined behaviour that prints checksums of what it computed.
# tests/x64fuzz.sh compiles it with mcc at several levels and with the C compiler of the x86-64 machine,
# and the outputs must be the same.
import random, sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
R = random.Random(seed)

ITYPES = ['signed char', 'unsigned char', 'short', 'unsigned short', 'int', 'unsigned', 'long', 'unsigned long']
FTYPES = ['float', 'double']
BITS = {'signed char': 8, 'unsigned char': 8, 'short': 16, 'unsigned short': 16, 'int': 32, 'unsigned': 32, 'long': 64, 'unsigned long': 64}
ALL = ITYPES + FTYPES

def is_f(t): return t in FTYPES
def is_u(t): return t.startswith('unsigned')
def prom(t):
    if is_f(t): return t
    return t if BITS[t] >= 32 else 'int'
def common(a, b):
    if is_f(a) or is_f(b):
        return 'double' if 'double' in (a, b) else 'float'
    a, b = prom(a), prom(b)
    if BITS[a] != BITS[b]: return a if BITS[a] > BITS[b] else b
    return a if is_u(a) else b
def uns(t): return {'int': 'unsigned', 'long': 'unsigned long'}.get(t, t)

def const(t):
    if is_f(t):
        return R.choice(['0.0', '1.0', '-1.5', '0.25', '3.75', '1e10', '-2.5e-3', '123456.789', '0.1']) + ('f' if t == 'float' else '')
    b = BITS[t]
    v = R.choice([0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 100, 127, 128, 255, 256, 1000, 32767, 32768, 65535, 65536,
                  0x7fffffff, 0x80000000, 0xffffffff, 0x100000000, 0x7fffffffffffffff, R.getrandbits(b), R.getrandbits(8)])
    v &= (1 << b) - 1
    if is_u(t):
        return '%du%s' % (v, 'l' if b == 64 else '')
    if v >= 1 << (b - 1): v -= 1 << b
    if b == 64:
        return '(-9223372036854775807l - 1)' if v == -(1 << 63) else '%dl' % v
    if v == -(1 << 31): return '(-2147483647 - 1)'
    return '(%s)%d' % (t, v) if b < 32 else str(v)

class Fn:
    def __init__(self, k, env):
        self.name = 'f%d' % k
        self.ret = R.choice(ALL)
        self.params = [(R.choice(ALL), 'p%d' % i) for i in range(R.randint(0, 7))]

class Gen:
    def __init__(self):
        self.out = []
        self.globals = []   # (type, name)
        self.arrays = []    # (type, name, n)
        self.fns = []     # read only: may be called anywhere in an expression
        self.procs = []   # write globals: called only as statements, so that the order of evaluation
                          # inside an expression never matters
        self.pure = True
        self.depth = 0
    def var(self, scope, t=None):
        c = [v for v in scope if t is None or v[0] == t]
        return R.choice(c) if c else None
    def expr(self, scope, t, d=0):
        """an expression of exactly type t"""
        return self.conv(scope, t, d, R.random() < 0.3)
    def conv(self, scope, t, d, cast=False):
        e, et = self.raw(scope, t, d)
        if et == t and not cast: return e
        if is_f(et) and not is_f(t):
            return '(%s)f2i(%s)' % (t, e)
        return '(%s)(%s)' % (t, e)
    def raw(self, scope, want, d):
        """(text, type) of some expression, preferring type want"""
        r = R.random()
        if d > 4 or r < 0.18:
            v = self.var(scope, want) or self.var(scope)
            if v and R.random() < 0.75: return v[1], v[0]
            return const(want), want if (is_f(want) or BITS[want] >= 32 or True) else want
        if r < 0.26 and self.arrays:
            t, n, k = R.choice(self.arrays)
            return '%s[(unsigned)(%s) %% %du]' % (n, self.expr(scope, 'unsigned', d + 1), k), t
        if r < 0.34 and self.fns and self.depth < 3:
            f = R.choice(self.fns)
            return '%s(%s)' % (f.name, ', '.join(self.expr(scope, pt, d + 1) for pt, _ in f.params)), f.ret
        if r < 0.40:
            a = self.expr(scope, 'int', d + 1)
            x, xt = self.raw(scope, want, d + 1); y = self.expr(scope, xt, d + 1)
            return '(%s ? %s : %s)' % (a, x, y), prom(xt) if not is_f(xt) else xt
        if r < 0.47:
            t2 = R.choice(ALL)
            e = self.expr(scope, t2, d + 1)
            if is_f(t2) and not is_f(want): return '(%s)f2i(%s)' % (want, e), want
            return '(%s)(%s)' % (want, e), want
        if r < 0.55:
            x, xt = self.raw(scope, want, d + 1)
            if is_f(xt): return '(-(%s))' % x, xt
            op = R.choice(['-', '~', '!'])
            if op == '!': return '(!(%s))' % x, 'int'
            pt = prom(xt)
            if op == '-': return '((%s)(0u - (%s)(%s)))' % (pt, uns(pt), x), pt
            return '(~(%s))' % x, pt
        a, at = self.raw(scope, want, d + 1)
        b, bt = self.raw(scope, R.choice([want, at, R.choice(ALL)]), d + 1)
        ct = common(at, bt)
        if is_f(ct):
            op = R.choice(['+', '-', '*', '/', '<', '<=', '>', '>=', '==', '!='])
            if op == '/': return 'fdiv((double)(%s), (double)(%s))' % (a, b), 'double'
            if op in '+-*': return '((%s)(%s) %s (%s)(%s))' % (ct, a, op, ct, b), ct
            return '((%s) %s (%s))' % (a, op, b), 'int'
        op = R.choice(['+', '-', '*', '&', '|', '^', '<<', '>>', '/', '%', '<', '<=', '>', '>=', '==', '!=', '&&', '||'])
        if op in ('&&', '||', '<', '<=', '>', '>=', '==', '!='):
            return '((%s) %s (%s))' % (a, op, b), 'int'
        if op in ('+', '-', '*'):
            u = uns(ct)
            return '((%s)((%s)(%s) %s (%s)(%s)))' % (ct, u, a, op, u, b), ct
        if op in ('&', '|', '^'):
            return '((%s) %s (%s))' % (a, op, b), ct
        if op in ('<<', '>>'):
            pt = prom(at)
            cnt = '((%s) & %d)' % (self.expr(scope, 'int', d + 1), BITS[pt] - 1)
            if op == '<<': return '((%s)((%s)(%s) << %s))' % (pt, uns(pt), a, cnt), pt
            return '((%s) >> %s)' % (a, cnt), pt
        fn = {'int': 'sdiv32', 'unsigned': 'udiv32', 'long': 'sdiv64', 'unsigned long': 'udiv64'}[ct]
        return '%s(%s, %s, %d)' % (fn, a, b, op == '%'), ct
    def stmts(self, scope, wr, ind, n, loopd=0):
        s = []
        for _ in range(n):
            r = R.random()
            pad = '    ' * ind
            if r < 0.45 and wr:
                t, v = R.choice(wr)
                op = R.choice(['=', '=', '=', '+=', '^=', '|=', '&=', '-=']) if not is_f(t) else '='
                if op in ('+=', '-='):
                    s.append('%s%s = (%s)((%s)%s %s (%s)%s);' % (pad, v, t, uns(prom(t)), v, op[0], uns(prom(t)), self.expr(scope, t)))
                else:
                    s.append('%s%s %s %s;' % (pad, v, op, self.expr(scope, t)))
            elif r < 0.50 and self.procs and not self.pure:
                f = R.choice(self.procs)
                s.append('%s%s(%s);' % (pad, f.name, ', '.join(self.expr(scope, pt) for pt, _ in f.params)))
            elif r < 0.55 and self.arrays and not self.pure:
                t, a, k = R.choice(self.arrays)
                s.append('%s%s[(unsigned)(%s) %% %du] = %s;' % (pad, a, self.expr(scope, 'unsigned'), k, self.expr(scope, t)))
            elif r < 0.72 and ind < 4:
                s.append('%sif (%s) {' % (pad, self.expr(scope, 'int')))
                s += self.stmts(scope, wr, ind + 1, R.randint(1, 3), loopd)
                if R.random() < 0.5:
                    s.append('%s} else {' % pad)
                    s += self.stmts(scope, wr, ind + 1, R.randint(1, 3), loopd)
                s.append('%s}' % pad)
            elif r < 0.84 and loopd < 2 and ind < 4:
                i = 'i%d' % loopd
                s.append('%sfor (%s = 0; %s < %d; %s++) {' % (pad, i, i, R.randint(1, 9), i))
                s += self.stmts(scope + [('int', i)], wr, ind + 1, R.randint(1, 3), loopd + 1)
                if R.random() < 0.2: s.append('%s    if (%s) break;' % (pad, self.expr(scope, 'int')))
                s.append('%s}' % pad)
            elif r < 0.92 and ind < 4:
                s.append('%sswitch ((%s) & 7) {' % (pad, self.expr(scope, 'int')))
                for c in R.sample(range(8), R.randint(1, 5)):
                    s.append('%scase %d:' % (pad, c))
                    s += self.stmts(scope, wr, ind + 1, R.randint(1, 2), loopd)
                    if R.random() < 0.8: s.append('%s    break;' % pad)
                if R.random() < 0.6:
                    s.append('%sdefault:' % pad)
                    s += self.stmts(scope, wr, ind + 1, 1, loopd)
                s.append('%s}' % pad)
            elif wr:
                t, v = R.choice(wr)
                if not is_f(t): s.append('%s%s = (%s)((%s)%s + 1u);' % (pad, v, t, uns(prom(t)), v))
        if not s:
            s.append('    ' * ind + ';')
        return s
    def function(self, k):
        f = Fn(k, self)
        self.pure = R.random() < 0.6
        loc = [(R.choice(ALL), 'v%d' % i) for i in range(R.randint(1, 6))]
        scope = f.params + loc + self.globals
        o = ['%s %s %s(%s)' % ('static' if R.random() < 0.6 else '', f.ret, f.name, ', '.join('%s %s' % p for p in f.params) or 'void'), '{']
        for t, v in loc:
            self.depth = 3
            o.append('    %s %s = %s;' % (t, v, self.expr(f.params + self.globals, t, 3)))
        self.depth = 0
        o.append('    int i0 = 0, i1 = 0;')
        o += self.stmts(scope, loc + f.params + ([] if self.pure else self.globals), 1, R.randint(2, 8))
        o.append('    (void)i0; (void)i1;')
        o.append('    return %s;' % self.expr(scope, f.ret))
        o.append('}')
        (self.fns if self.pure else self.procs).append(f)
        return o
    def program(self):
        o = ['#include <stdio.h>', '#include <string.h>',
             'static int sdiv32(int a, int b, int m) { if (b == 0 || (a == -2147483647 - 1 && b == -1)) return a; return m ? a % b : a / b; }',
             'static unsigned udiv32(unsigned a, unsigned b, int m) { if (b == 0) return a; return m ? a % b : a / b; }',
             'static long sdiv64(long a, long b, int m) { if (b == 0 || (a == -9223372036854775807l - 1 && b == -1)) return a; return m ? a % b : a / b; }',
             'static unsigned long udiv64(unsigned long a, unsigned long b, int m) { if (b == 0) return a; return m ? a % b : a / b; }',
             'static double fdiv(double a, double b) { return b == 0 ? a : a / b; }',
             'static long f2i(double x) { return x > -1e15 && x < 1e15 ? (long)x : 0; }',
             'static unsigned long sum;',
             'static void mix(unsigned long v) { sum = (sum ^ v) * 1099511628211ul + (sum >> 29); }',
             'static void mixd(double d) { unsigned long v; if (d != d) { mix(77); return; } if (d == 0) d = 0; memcpy(&v, &d, 8); mix(v); }']
        for i in range(R.randint(4, 12)):
            t = R.choice(ALL)
            self.globals.append((t, 'g%d' % i))
            o.append('%s%s g%d = %s;' % ('static ' if R.random() < 0.5 else '', t, i, const(t)))
        for i in range(R.randint(1, 4)):
            t = R.choice(ALL); n = R.randint(1, 20)
            self.arrays.append((t, 'a%d' % i, n))
            o.append('static %s a%d[%d];' % (t, i, n))
        for k in range(R.randint(3, 12)):
            o += self.function(k)
        o.append('int main(void)\n{\n    int r, k;')
        o.append('    for (r = 0; r < 3; r++) {')
        for f in self.fns + self.procs:
            self.depth = 3
            call = '%s(%s)' % (f.name, ', '.join(self.expr(self.globals, pt, 3) for pt, _ in f.params))
            o.append('        %s(%s);' % ('mixd' if is_f(f.ret) else 'mix', ('(double)' if is_f(f.ret) else '(unsigned long)') + call))
            o.append('        printf("%s %%d %%lx\\n", r, sum);' % f.name)
        o.append('    }')
        for t, g in self.globals:
            o.append('    %s(%s%s);' % ('mixd' if is_f(t) else 'mix', '(double)' if is_f(t) else '(unsigned long)', g))
        for t, a, n in self.arrays:
            o.append('    for (k = 0; k < %d; k++) %s(%s%s[k]);' % (n, 'mixd' if is_f(t) else 'mix', '(double)' if is_f(t) else '(unsigned long)', a))
        o.append('    printf("sum %lx\\n", sum);\n    return 0;\n}')
        return '\n'.join(o) + '\n'

sys.stdout.write(Gen().program())
