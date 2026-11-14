#!/usr/bin/env python3
# gen.py DIR: writes the generated tests g_*.ptx and what they must print, g_*.exp, into DIR. The expected
# values come from the reference semantics written here (exact rational arithmetic for floating point), not
# from the engine. The files it wrote are kept in tests/cuda; this is how to make them again.
# The same files run on a real GPU (gpuptx.sh) print the same: where the hardware and the manual differ, the
# hardware is what is written here (gpuptx.txt has the list).
import sys, struct, math, re
from fractions import Fraction

M32 = 0xffffffff
M64 = 0xffffffffffffffff

def s32(x): x &= M32; return x - (1 << 32) if x >> 31 else x
def s64(x): x &= M64; return x - (1 << 64) if x >> 63 else x
def sx(x, bits): x &= (1 << bits) - 1; return x - (1 << bits) if x >> (bits - 1) else x

class T:
    def __init__(self, name, target='sm_86', version='8.5'):
        self.name = name
        self.target = target
        self.version = version
        self.lines = []
        self.exp = []
        self.parts = []
    # A constant goes through an exclusive or with a zero read from memory: the hardware's assembler works out
    # at build time what it can (and not always the way the hardware does it, e.g. a tie in sub.rn.f32), and
    # this is about the instructions.
    def opaque(self, m):
        ty = m.group(1); r = m.group(2)
        if ty == 'b32': return m.group(0) + ' xor.b32 %s, %s, %%z;' % (r, r)
        if ty == 'b64': return m.group(0) + ' xor.b64 %s, %s, %%zl;' % (r, r)
        if ty == 'b16': return m.group(0) + ' xor.b16 %s, %s, %%zh;' % (r, r)
        if ty == 'f32': return m.group(0) + ' mov.b32 %%zt, %s; xor.b32 %%zt, %%zt, %%z; mov.b32 %s, %%zt;' % (r, r)
        return m.group(0) + ' mov.b64 %%zu, %s; xor.b64 %%zu, %%zu, %%zl; mov.b64 %s, %%zu;' % (r, r)
    def w(self, s): self.lines.append('    ' + re.sub(r'mov\.(b16|b32|b64|f32|f64) (%\w+), (?:0[xfdXFD][0-9A-Fa-f]+|\d+);', self.opaque, s))
    # A kernel ends here if it is long enough (where nothing is carried over in a register): the hardware's
    # assembler takes long over a very long kernel, and beyond some length was seen to get an instruction wrong
    # (cvt.rni.s32.f64 of +inf gave 0 in a kernel of 10270 lines and 0x7fffffff, as it must, in a shorter one).
    def cut(self, limit=1500):
        if len(self.lines) >= limit:
            self.parts.append((self.lines, self.exp))
            self.lines = []; self.exp = []
    def out32(self, reg, val):
        self.w('st.global.b32 [%%ro+%d], %s;' % (4 * len(self.exp), reg))
        self.exp.append(val & M32)
    def out64(self, reg, val):
        if len(self.exp) & 1:
            self.exp.append(0)
        self.w('st.global.b64 [%%ro+%d], %s;' % (4 * len(self.exp), reg))
        self.exp.append(val & M32)
        self.exp.append(val >> 32 & M32)
    def outp(self, val):
        self.w('selp.b32 %x, 1, 0, %p;')
        self.out32('%x', val)
    def finish(self, d):
        self.cut(1)
        f = open('%s/g_%s.ptx' % (d, self.name), 'w')
        for (i, (lines, exp)) in enumerate(self.parts):
            f.write('//! run k%d 1 1 1 1 1 1 0 %d\n' % (i, len(exp)))
        f.write('// made by gen.py\n.version %s\n.target %s\n.address_size 64\n' % (self.version, self.target))
        for (i, (lines, exp)) in enumerate(self.parts):
            f.write('\n.visible .entry k%d(.param .u64 out)\n{\n' % i)
            f.write('    .reg .b64 %ro;\n    .reg .b32 %a, %b, %c, %e, %x;\n    .reg .b64 %la, %lb, %lc, %le, %lx;\n')
            f.write('    .reg .b16 %ha, %hb, %hc, %hx;\n    .reg .f32 %fa, %fb, %fc, %fx;\n    .reg .f64 %da, %db, %dc, %dx;\n')
            f.write('    .reg .pred %p, %q;\n    .reg .b32 %z, %zt;\n    .reg .b64 %zl, %zu;\n    .reg .b16 %zh;\n    ld.param.u64 %ro, [out];\n')
            f.write('    ld.global.u32 %z, [%ro];\n    cvt.u64.u32 %zl, %z;\n    cvt.u16.u32 %zh, %z;\n')
            f.write('\n'.join(lines) + '\n    ret;\n}\n')
        f.close()
        f = open('%s/g_%s.exp' % (d, self.name), 'w')
        for (k, (lines, exp)) in enumerate(self.parts):
            f.write('k%d: 0\n' % k)
            n = len(exp)
            for i in range(n):
                f.write('%08x%s' % (exp[i], '\n' if i % 8 == 7 or i == n - 1 else ' '))
        f.close()

# ---------------- integers ----------------

V32 = [0, 1, 2, 7, 31, 32, 33, 100, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678, 0xdeadbeef, 0xfffffff9, 0x00ff00ff, 0x80000001]
V64 = [0, 1, 63, 64, 65, 0x7fffffffffffffff, 0x8000000000000000, M64, 0x123456789abcdef0, 0xdeadbeefcafebabe, 0xfffffffffffffff9, 0x100000000]

def clz(x, bits): return bits - x.bit_length()
def bfind(x, bits, signed, shiftamt):
    if signed and x >> (bits - 1): x = ~x & ((1 << bits) - 1)
    if x == 0: return M32
    d = x.bit_length() - 1
    return bits - 1 - d if shiftamt else d
def brev(x, bits): return int(bin(x)[2:].zfill(bits)[::-1], 2)
def bfe(a, b, c, bits, signed):
    pos = b & 0xff; ln = c & 0xff; msb = bits - 1
    sbit = 0
    if signed and ln: sbit = a >> min(pos + ln - 1, msb) & 1
    d = 0
    for i in range(bits):
        bit = (a >> (pos + i) & 1) if (i < ln and pos + i <= msb) else sbit
        d |= bit << i
    return d
def bfi(a, b, c, e, bits):
    pos = c & 0xff; ln = e & 0xff; f = b
    for i in range(ln):
        if pos + i < bits:
            f = f & ~(1 << (pos + i)) | (a >> i & 1) << (pos + i)
    return f
def shl(a, b, bits): return 0 if b >= bits else a << b & ((1 << bits) - 1)
def shr(a, b, bits, signed):
    if signed: return (sx(a, bits) >> min(b, bits - 1)) & ((1 << bits) - 1)
    return 0 if b >= bits else a >> b
def div(a, b, bits, signed):
    if signed:
        a = sx(a, bits); b = sx(b, bits)
        q = abs(a) // abs(b)
        return -q if (a < 0) != (b < 0) else q
    return a // b
def rem(a, b, bits, signed):
    if signed:
        a = sx(a, bits); b = sx(b, bits)
        r = abs(a) % abs(b)
        return -r if a < 0 else r
    return a % b
def mul24(a, b, signed, hi):
    if signed: p = sx(a, 24) * sx(b, 24)
    else: p = (a & 0xffffff) * (b & 0xffffff)
    p &= (1 << 48) - 1
    return p >> 16 if hi else p
def fns(mask, base, off):
    off = s32(off)
    if base > 31: return M32
    if off == 0: return base if mask >> base & 1 else M32
    pos = base; count = abs(off) - 1; inc = 1 if off > 0 else -1
    while 0 <= pos < 32:
        if mask >> pos & 1:
            if count == 0: return pos
            count -= 1
        pos += inc
    return M32
def szext(a, b, clamp, signed):
    b1 = b & 31; too = b >= 32 and clamp
    mask = 0 if too else (M32 << b1) & M32
    sp = (b1 - 1) & 31
    sb = 0 if (b1 == 0 or too or not signed) else a >> sp & 1
    return (a & ~mask & M32) | (mask if sb else 0)
def bmsk(a, b, clamp):
    a1 = a & 31; mask0 = (M32 << a1) & M32; b1 = b & 31; sm = a1 + b1
    mask1 = (M32 << sm) & M32
    so = sm >= 32; po = wo = False
    if clamp:
        if a >= 32: po = True; mask0 = 0
        if b >= 32: wo = True
    if so or po or wo: mask1 = 0
    elif b1 == 0: mask1 = M32
    return mask0 & ~mask1 & M32
def bytes4(x, signed): return [sx(x >> 8 * i, 8) if signed else x >> 8 * i & 0xff for i in range(4)]
def dp4a(a, b, c, sa, sb):
    va = bytes4(a, sa); vb = bytes4(b, sb)
    return c + sum(va[i] * vb[i] for i in range(4))
def dp2a(a, b, c, sa, sb, hi):
    va = [sx(a >> 16 * i, 16) if sa else a >> 16 * i & 0xffff for i in range(2)]; vb = bytes4(b, sb)
    return c + sum(va[i] * vb[(2 if hi else 0) + i] for i in range(2))
def shf(a, b, c, left, clamp):
    n = min(c, 32) if clamp else c & 31
    v = b << 32 | a
    return (v << n >> 32 if left else v >> n) & M32
def prmt(a, b, c, mode):
    src = [(b << 32 | a) >> 8 * i & 0xff for i in range(8)]
    s = c & 3
    if mode == '':
        d = 0
        for i in range(4):
            sel = c >> 4 * i & 15
            v = src[sel & 7]
            if sel & 8: v = 0xff if v & 0x80 else 0
            d |= v << 8 * i
        return d
    tab = {'f4e': [[0, 1, 2, 3], [1, 2, 3, 4], [2, 3, 4, 5], [3, 4, 5, 6]],
           'b4e': [[0, 7, 6, 5], [1, 0, 7, 6], [2, 1, 0, 7], [3, 2, 1, 0]],
           'rc8': [[0] * 4, [1] * 4, [2] * 4, [3] * 4],
           'ecl': [[0, 1, 2, 3], [1, 1, 2, 3], [2, 2, 2, 3], [3, 3, 3, 3]],
           'ecr': [[0, 0, 0, 0], [0, 1, 1, 1], [0, 1, 2, 2], [0, 1, 2, 3]],
           'rc16': [[0, 1, 0, 1], [2, 3, 2, 3], [0, 1, 0, 1], [2, 3, 2, 3]]}[mode][s]
    return sum(src[tab[i]] << 8 * i for i in range(4))

def gen_int(d):
    t = T('int32')
    pairs = [(a, b) for a in V32 for b in V32]
    for (a, b) in pairs:
        t.cut()
        t.w('mov.b32 %%a, 0x%x; mov.b32 %%b, 0x%x;' % (a, b))
        def op(name, val):
            t.w('%s %%x, %%a, %%b;' % name)
            t.out32('%x', val)
        op('add.s32', a + b); op('sub.u32', a - b); op('mul.lo.s32', a * b)
        op('mul.hi.u32', a * b >> 32); op('mul.hi.s32', s32(a) * s32(b) >> 32)
        op('and.b32', a & b); op('or.b32', a | b); op('xor.b32', a ^ b)
        op('min.s32', min(s32(a), s32(b))); op('max.s32', max(s32(a), s32(b)))
        op('min.u32', min(a, b)); op('max.u32', max(a, b))
        op('shl.b32', shl(a, b, 32)); op('shr.u32', shr(a, b, 32, 0)); op('shr.s32', shr(a, b, 32, 1))
        op('mul24.lo.u32', mul24(a, b, 0, 0)); op('mul24.lo.s32', mul24(a, b, 1, 0))
        op('mul24.hi.u32', mul24(a, b, 0, 1)); op('mul24.hi.s32', mul24(a, b, 1, 1))
        if b:
            op('div.u32', div(a, b, 32, 0)); op('rem.u32', rem(a, b, 32, 0))
            if not (a == 0x80000000 and b == M32):
                op('div.s32', div(a, b, 32, 1)); op('rem.s32', rem(a, b, 32, 1))
        sat = max(-2**31, min(2**31 - 1, s32(a) + s32(b)))
        op('add.sat.s32', sat)
        sat = max(-2**31, min(2**31 - 1, s32(a) - s32(b)))
        op('sub.sat.s32', sat)
        t.w('mul.wide.u32 %lx, %a, %b;'); t.out64('%lx', a * b)
        t.w('mul.wide.s32 %lx, %a, %b;'); t.out64('%lx', s32(a) * s32(b) & M64)
        for nm in ['eq', 'ne', 'lt', 'le', 'gt', 'ge', 'lo', 'ls', 'hi', 'hs']:
            for ty in ['s32', 'u32']:
                if ty == 's32' and nm in ['lo', 'ls', 'hi', 'hs']: continue
                x, y = (s32(a), s32(b)) if ty == 's32' else (a, b)
                r = {'eq': x == y, 'ne': x != y, 'lt': x < y, 'le': x <= y, 'gt': x > y, 'ge': x >= y,
                     'lo': x < y, 'ls': x <= y, 'hi': x > y, 'hs': x >= y}[nm]
                t.w('setp.%s.%s %%p, %%a, %%b;' % (nm, ty)); t.outp(int(r))
        t.w('set.lt.u32.s32 %x, %a, %b;'); t.out32('%x', M32 if s32(a) < s32(b) else 0)
        # an immediate operand
        t.w('add.u32 %%x, %%a, %d;' % s32(b)); t.out32('%x', a + b)
    for a in V32:
        t.cut()
        t.w('mov.b32 %%a, 0x%x;' % a)
        def op1(name, val):
            t.w('%s %%x, %%a;' % name)
            t.out32('%x', val)
        op1('not.b32', ~a); op1('neg.s32', -a); op1('abs.s32', abs(s32(a))); op1('cnot.b32', int(a == 0))
        op1('popc.b32', bin(a).count('1')); op1('clz.b32', clz(a, 32)); op1('brev.b32', brev(a, 32))
        op1('bfind.u32', bfind(a, 32, 0, 0)); op1('bfind.s32', bfind(a, 32, 1, 0))
        op1('bfind.shiftamt.u32', bfind(a, 32, 0, 1)); op1('bfind.shiftamt.s32', bfind(a, 32, 1, 1))
    tri = [(a, b, c) for a in [0x12345678, 0xdeadbeef, 0x80ff7f01, 0xffffffff] for b in [0, 3, 0x89abcdef, 31, 0xffff8001]
           for c in [0, 1, 5, 32, 33, 0x3210, 0xba98, 0xfffffffe, 0x7654]]
    for (a, b, c) in tri:
        t.cut()
        t.w('mov.b32 %%a, 0x%x; mov.b32 %%b, 0x%x; mov.b32 %%c, 0x%x;' % (a, b, c))
        def op3(name, val):
            t.w('%s %%x, %%a, %%b, %%c;' % name)
            t.out32('%x', val)
        op3('mad.lo.s32', a * b + c); op3('mad.hi.u32', (a * b >> 32) + c); op3('mad.hi.s32', (s32(a) * s32(b) >> 32) + c)
        op3('mad24.lo.u32', mul24(a, b, 0, 0) + c); op3('mad24.hi.s32', mul24(a, b, 1, 1) + c)
        op3('sad.u32', abs(a - b) + c); op3('sad.s32', abs(s32(a) - s32(b)) + c)
        op3('bfe.u32', bfe(a, b, c, 32, 0)); op3('bfe.s32', bfe(a, b, c, 32, 1))
        for (sa, sb) in [(0, 0), (0, 1), (1, 0), (1, 1)]:
            ty = '.%s32.%s32' % ('us'[sa], 'us'[sb])
            op3('dp4a' + ty, dp4a(a, b, c, sa, sb)); op3('dp2a.lo' + ty, dp2a(a, b, c, sa, sb, 0)); op3('dp2a.hi' + ty, dp2a(a, b, c, sa, sb, 1))
        for left in (0, 1):
            for clamp in (0, 1):
                op3('shf.%s.%s.b32' % ('rl'[left], ['wrap', 'clamp'][clamp]), shf(a, b, c, left, clamp))
        for mode in ['', 'f4e', 'b4e', 'rc8', 'ecl', 'ecr', 'rc16']:
            op3('prmt.b32' + ('.' + mode if mode else ''), prmt(a, b, c, mode))
        op3('fns.b32', fns(a, b, c)); t.w('neg.s32 %e, %c;'); t.w('fns.b32 %x, %a, %b, %e;')
        t.out32('%x', fns(a, b, -s32(c)))
        op3('slct.b32.s32', a if s32(c) >= 0 else b)
        t.w('mul.wide.u32 %lc, %c, %c;'); t.w('mad.wide.u32 %lx, %a, %b, %lc;'); t.out64('%lx', (a * b + c * c) & M64)
        t.w('mad.wide.s32 %lx, %a, %b, %lc;'); t.out64('%lx', (s32(a) * s32(b) + c * c) & M64)
        for i, lut in enumerate([0x00, 0x80, 0xe8, 0x96, 0xca, 0xfe, 0x1b, 0xff]):
            v = 0
            for k in range(32):
                idx = (a >> k & 1) << 2 | (b >> k & 1) << 1 | (c >> k & 1)
                v |= (lut >> idx & 1) << k
            t.w('lop3.b32 %%x, %%a, %%b, %%c, 0x%x;' % lut); t.out32('%x', v)
        t.w('mov.b32 %%e, 0x%x;' % (c & 0xff)); t.w('bfi.b32 %x, %a, %b, %c, %e;'); t.out32('%x', bfi(a, b, c, c & 0xff, 32))
        t.w('mov.b32 %e, 7;'); t.w('bfi.b32 %x, %a, %b, %c, %e;'); t.out32('%x', bfi(a, b, c, 7, 32))
    for a in [0, 1, 5, 31, 32, 40, 0x12345678, 0xffffffff, 0x8000]:
        t.cut()
        for b in [0, 1, 8, 16, 31, 32, 33, 0x107]:
            t.w('mov.b32 %%a, 0x%x; mov.b32 %%b, 0x%x;' % (a, b))
            for clamp in (0, 1):
                md = ['wrap', 'clamp'][clamp]
                t.w('szext.%s.s32 %%x, %%a, %%b;' % md); t.out32('%x', szext(a, b, clamp, 1))
                t.w('szext.%s.u32 %%x, %%a, %%b;' % md); t.out32('%x', szext(a, b, clamp, 0))
                t.w('bmsk.%s.b32 %%x, %%a, %%b;' % md); t.out32('%x', bmsk(a, b, clamp))
    # carries
    for (a, b) in [(0xffffffff, 1), (5, 7), (0x80000000, 0x80000000), (0, 0), (3, 0xfffffffd)]:
        t.cut()
        for (c, e) in [(0, 0), (0xffffffff, 0), (1, 0xffffffff)]:
            t.w('mov.b32 %%a, 0x%x; mov.b32 %%b, 0x%x; mov.b32 %%c, 0x%x; mov.b32 %%e, 0x%x;' % (a, b, c, e))
            lo = a + b; hi = c + e + (lo >> 32)
            t.w('add.cc.u32 %x, %a, %b;'); t.out32('%x', lo)
            t.w('addc.cc.u32 %x, %c, %e;'); t.out32('%x', hi)
            t.w('addc.u32 %x, %a, %a;'); t.out32('%x', a + a + (hi >> 32))
            lo = a - b; bor = int(a < b); hi = c - e - bor; bor2 = int(c < e + bor)
            t.w('sub.cc.u32 %x, %a, %b;'); t.out32('%x', lo)
            t.w('subc.cc.u32 %x, %c, %e;'); t.out32('%x', hi)
            t.w('subc.u32 %x, %a, %a;'); t.out32('%x', -bor2)
    t.finish(d)

    t = T('int64')
    for a in V64:
        t.cut()
        for b in V64:
            t.w('mov.b64 %%la, 0x%x; mov.b64 %%lb, 0x%x;' % (a, b))
            def op(name, val):
                t.w('%s %%lx, %%la, %%lb;' % name)
                t.out64('%lx', val & M64)
            op('add.s64', a + b); op('sub.s64', a - b); op('mul.lo.u64', a * b)
            op('mul.hi.u64', a * b >> 64); op('mul.hi.s64', s64(a) * s64(b) >> 64)
            op('and.b64', a & b); op('or.b64', a | b); op('xor.b64', a ^ b)
            op('min.s64', min(s64(a), s64(b))); op('max.s64', max(s64(a), s64(b)))
            op('min.u64', min(a, b)); op('max.u64', max(a, b))
            if b:
                op('div.u64', div(a, b, 64, 0)); op('rem.u64', rem(a, b, 64, 0))
                if not (a == 1 << 63 and b == M64):
                    op('div.s64', div(a, b, 64, 1)); op('rem.s64', rem(a, b, 64, 1))
            t.w('cvt.u32.u64 %b, %lb;')
            for (nm, v) in [('shl.b64', shl(a, b & M32, 64)), ('shr.u64', shr(a, b & M32, 64, 0)), ('shr.s64', shr(a, b & M32, 64, 1))]:
                t.w('%s %%lx, %%la, %%b;' % nm); t.out64('%lx', v)
            for nm in ['eq', 'ne', 'lt', 'le', 'gt', 'ge', 'lo', 'ls', 'hi', 'hs']:
                for ty in ['s64', 'u64']:
                    if ty == 's64' and nm in ['lo', 'ls', 'hi', 'hs']: continue
                    x, y = (s64(a), s64(b)) if ty == 's64' else (a, b)
                    r = {'eq': x == y, 'ne': x != y, 'lt': x < y, 'le': x <= y, 'gt': x > y, 'ge': x >= y,
                         'lo': x < y, 'ls': x <= y, 'hi': x > y, 'hs': x >= y}[nm]
                    t.w('setp.%s.%s %%p, %%la, %%lb;' % (nm, ty)); t.outp(int(r))
            t.w('mad.lo.u64 %lx, %la, %lb, %la;'); t.out64('%lx', (a * b + a) & M64)
            t.w('mad.hi.s64 %lx, %la, %lb, %la;'); t.out64('%lx', ((s64(a) * s64(b) >> 64) + a) & M64)
            t.w('add.cc.u64 %lx, %la, %lb;'); t.out64('%lx', (a + b) & M64)
            t.w('addc.u64 %lx, %la, 0;'); t.out64('%lx', (a + (a + b >> 64)) & M64)
            t.w('sub.cc.u64 %lx, %la, %lb;'); t.out64('%lx', (a - b) & M64)
            t.w('subc.u64 %lx, %la, 0;'); t.out64('%lx', (a - int(a < b)) & M64)
            t.w('mov.b32 %a, 12; mov.b32 %c, 40;')
            t.w('bfe.u64 %lx, %la, %a, %c;'); t.out64('%lx', bfe(a, 12, 40, 64, 0))
            t.w('bfe.s64 %lx, %la, %c, %c;'); t.out64('%lx', bfe(a, 40, 40, 64, 1))
            t.w('bfi.b64 %lx, %la, %lb, %a, %c;'); t.out64('%lx', bfi(a, b, 12, 40, 64))
        def op1(name, val, wide):
            t.w('%s %s, %%la;' % (name, '%lx' if wide else '%x'))
            if wide: t.out64('%lx', val & M64)
            else: t.out32('%x', val)
        op1('not.b64', ~a, 1); op1('neg.s64', -a, 1); op1('abs.s64', abs(s64(a)), 1); op1('cnot.b64', int(a == 0), 1)
        op1('popc.b64', bin(a).count('1'), 0); op1('clz.b64', clz(a, 64), 0); op1('brev.b64', brev(a, 64), 1)
        op1('bfind.u64', bfind(a, 64, 0, 0), 0); op1('bfind.s64', bfind(a, 64, 1, 0), 0)
        op1('bfind.shiftamt.u64', bfind(a, 64, 0, 1), 0); op1('bfind.shiftamt.s64', bfind(a, 64, 1, 1), 0)
    t.finish(d)

    t = T('int16')
    V16 = [0, 1, 0x7fff, 0x8000, 0xffff, 0x1234, 0xfff9, 300]
    for a in V16:
        t.cut()
        for b in V16:
            t.w('mov.b16 %%ha, 0x%x; mov.b16 %%hb, 0x%x;' % (a, b))
            def op(name, val):
                t.w('%s %%hx, %%ha, %%hb;' % name)
                t.w('cvt.u32.u16 %x, %hx;')
                t.out32('%x', val & 0xffff)
            op('add.s16', a + b); op('sub.u16', a - b); op('mul.lo.s16', a * b)
            op('mul.hi.u16', a * b >> 16); op('mul.hi.s16', sx(a, 16) * sx(b, 16) >> 16)
            op('and.b16', a & b); op('or.b16', a | b); op('xor.b16', a ^ b)
            op('min.s16', min(sx(a, 16), sx(b, 16))); op('max.s16', max(sx(a, 16), sx(b, 16)))
            op('min.u16', min(a, b)); op('max.u16', max(a, b))
            if b:
                op('div.u16', div(a, b, 16, 0)); op('rem.u16', rem(a, b, 16, 0))
                if not (a == 0x8000 and b == 0xffff):
                    op('div.s16', div(a, b, 16, 1)); op('rem.s16', rem(a, b, 16, 1))
            t.w('mov.b32 %%b, %d;' % (b & 0x1f))
            for (nm, v) in [('shl.b16', shl(a, b & 0x1f, 16)), ('shr.u16', shr(a, b & 0x1f, 16, 0)), ('shr.s16', shr(a, b & 0x1f, 16, 1))]:
                t.w('%s %%hx, %%ha, %%b;' % nm); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', v)
            t.w('mul.wide.u16 %x, %ha, %hb;'); t.out32('%x', a * b)
            t.w('mul.wide.s16 %x, %ha, %hb;'); t.out32('%x', sx(a, 16) * sx(b, 16))
            for nm in ['eq', 'ne', 'lt', 'le', 'gt', 'ge']:
                for ty in ['s16', 'u16']:
                    x, y = (sx(a, 16), sx(b, 16)) if ty == 's16' else (a, b)
                    r = {'eq': x == y, 'ne': x != y, 'lt': x < y, 'le': x <= y, 'gt': x > y, 'ge': x >= y}[nm]
                    t.w('setp.%s.%s %%p, %%ha, %%hb;' % (nm, ty)); t.outp(int(r))
        for (nm, v) in [('not.b16', ~a), ('neg.s16', -a), ('abs.s16', abs(sx(a, 16)))]:
            t.w('%s %%hx, %%ha;' % nm); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', v & 0xffff)
        t.w('cvt.s32.s16 %x, %ha;'); t.out32('%x', sx(a, 16))
        t.w('cvt.s64.s16 %lx, %ha;'); t.out64('%lx', sx(a, 16) & M64)
    t.finish(d)

# ---------------- floating point ----------------

class Fmt:
    def __init__(self, mbits, ebits, noinf=False, nonan=False):
        self.nonan = nonan
        self.m = mbits; self.e = ebits; self.bias = (1 << (ebits - 1)) - 1; self.bits = 1 + ebits + mbits
        self.noinf = noinf
    def inf(self, sign): return sign << (self.bits - 1) | ((1 << self.e) - 1) << self.m
    def nan(self): return ((1 << self.e) - 1) << self.m | 1 << (self.m - 1)
    def maxfin(self, sign):
        if self.noinf: return sign << (self.bits - 1) | ((1 << (self.bits - 1)) - (1 if self.nonan else 2))
        return self.inf(sign) - 1
    def dec(self, b):
        """bits -> ('nan'|'inf'|'num', sign, Fraction)"""
        sign = b >> (self.bits - 1) & 1; ex = b >> self.m & ((1 << self.e) - 1); fr = b & ((1 << self.m) - 1)
        if self.noinf:
            if not self.nonan and b & ((1 << (self.bits - 1)) - 1) == (1 << (self.bits - 1)) - 1: return ('nan', sign, None)
        elif ex == (1 << self.e) - 1:
            return ('nan', sign, None) if fr else ('inf', sign, None)
        if ex == 0: v = Fraction(fr, 1 << self.m) * Fraction(2) ** (1 - self.bias)
        else: v = (1 + Fraction(fr, 1 << self.m)) * Fraction(2) ** (ex - self.bias)
        return ('num', sign, -v if sign else v)
    def enc(self, x, mode='rn', zsign=0, sat=False):
        """Fraction -> bits, rounded as mode says (rn, rz, rm, rp, rna)"""
        if x == 0: return zsign << (self.bits - 1)
        sign = int(x < 0); a = abs(x)
        e = a.numerator.bit_length() - a.denominator.bit_length()
        if Fraction(2) ** e > a: e -= 1
        emin = 1 - self.bias
        if e < emin: e = emin
        sc = a / Fraction(2) ** (e - self.m)
        q = sc.numerator // sc.denominator; r = sc - q
        up = False
        if r:
            if mode == 'rn': up = r > Fraction(1, 2) or (r == Fraction(1, 2) and q & 1)
            elif mode == 'rna': up = r >= Fraction(1, 2)
            elif mode == 'rz': up = False
            elif mode == 'rm': up = bool(sign)
            elif mode == 'rp': up = not sign
        q += up
        if q == 1 << (self.m + 1): q >>= 1; e += 1
        top = self.bias if not self.noinf else self.bias + 1
        over = e > top or (self.noinf and not self.nonan and e == top and q == (1 << (self.m + 1)) - 1)
        if over:
            if sat or self.noinf: return self.maxfin(sign)
            if mode in ('rn', 'rna') or (mode == 'rm' and sign) or (mode == 'rp' and not sign): return self.inf(sign)
            return self.maxfin(sign)
        if q < 1 << self.m: return sign << (self.bits - 1) | q
        return sign << (self.bits - 1) | (e + self.bias) << self.m | (q - (1 << self.m))

F32 = Fmt(23, 8); F64 = Fmt(52, 11); F16 = Fmt(10, 5); BF16 = Fmt(7, 8)
E4M3 = Fmt(3, 4, True); E5M2 = Fmt(2, 5); E2M1 = Fmt(1, 2, True, True)

def f32b(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def f64b(x): return struct.unpack('<Q', struct.pack('<d', x))[0]

def fsqrt(x):
    """a rational close enough to the square root that rounding it rounds the root (never a tie)"""
    k = 1300
    n = (x * (1 << (2 * k))).numerator // (x * (1 << (2 * k))).denominator
    s = math.isqrt(n)
    exact = s * s == n and (x * (1 << (2 * k))).denominator == 1
    return Fraction(s, 1 << k) if exact else Fraction(2 * s + 1, 1 << (k + 1))

def arith(fmt, op, mode, a, b, c=0):
    """a, b, c are bit patterns of finite numbers; the result's bits, or None if it is not worth pinning down"""
    (_, sa, xa) = fmt.dec(a); (_, sb, xb) = fmt.dec(b); (_, sc, xc) = fmt.dec(c)
    zs = 0
    if op == 'add' or op == 'sub':
        if op == 'sub': xb = -xb; sb ^= 1
        r = xa + xb
        zs = sa if (xa == 0 and xb == 0 and sa == sb) else (1 if mode == 'rm' else 0)
    elif op == 'mul': r = xa * xb; zs = sa ^ sb
    elif op == 'div':
        if xb == 0: return None
        r = xa / xb; zs = sa ^ sb
    elif op == 'fma':
        r = xa * xb + xc
        if r == 0:
            ps = sa ^ sb
            if xa * xb == 0 and xc == 0: zs = ps if ps == sc else (1 if mode == 'rm' else 0)
            else: zs = 1 if mode == 'rm' else 0
    elif op == 'sqrt':
        if xa < 0: return None
        if xa == 0: return a
        r = fsqrt(xa)
    elif op == 'rcp':
        if xa == 0: return None
        r = 1 / xa; zs = sa
    return fmt.enc(r, mode, zs)

VF32 = [1.0, 1.5, 3.0, 0.1, 1e-3, 16777216.0, 16777215.0, 1e30, 1e-30, 1.17549435e-38, 1e-45, 3.4028235e38, -2.5, 7.0, 1 / 3.0,
        -0.0, 0.0, -1e-40, 5e-39, -3.4028235e38, 123456.789]
VF64 = [1.0, 1.5, 3.0, 0.1, 1e-3, 9007199254740992.0, 9007199254740991.0, 1e300, 1e-300, 2.2250738585072014e-308, 5e-324,
        1.7976931348623157e308, -2.5, 7.0, 1 / 3.0, -0.0, 0.0, -1e-310, -1.7976931348623157e308, 123456.789]

def gen_flt(d):
    for (nm, fmt, vals, ty, reg, lit) in [('f32', F32, [f32b(v) for v in VF32], 'f32', 'f', '0f%08X'),
                                          ('f64', F64, [f64b(v) for v in VF64], 'f64', 'd', '0d%016X')]:
        t = T(nm)
        out = t.out32 if ty == 'f32' else t.out64
        bw = 'b32' if ty == 'f32' else 'b64'
        ra, rb, rc, rx = ['%' + reg + k for k in 'abcx']
        for a in vals:
            t.cut()
            for b in vals:
                t.w(('mov.%s %s, ' + lit + '; mov.%s %s, ' + lit + ';') % (ty, ra, a, ty, rb, b))
                for op in ['add', 'sub', 'mul', 'div']:
                    for mode in ['rn', 'rz', 'rm', 'rp']:
                        r = arith(fmt, op, mode, a, b)
                        if r is None: continue
                        t.w('%s.%s.%s %s, %s, %s;' % (op, mode, ty, rx, ra, rb)); out(rx, r)
                r = arith(fmt, 'mul', 'rn', a, b)
                t.w('mul.%s %s, %s, %s;' % (ty, rx, ra, rb)); out(rx, r)
                (_, sa, xa) = fmt.dec(a); (_, sb, xb) = fmt.dec(b)
                # min and max of a zero and a zero of the other sign are left alone
                if not (xa == 0 and xb == 0):
                    t.w('min.%s %s, %s, %s;' % (ty, rx, ra, rb)); out(rx, a if xa <= xb else b)
                    t.w('max.%s %s, %s, %s;' % (ty, rx, ra, rb)); out(rx, a if xa >= xb else b)
                t.w('copysign.%s %s, %s, %s;' % (ty, rx, ra, rb))
                out(rx, (b & ~(1 << (fmt.bits - 1))) | (a & (1 << (fmt.bits - 1))))
                for (cn, f) in [('eq', xa == xb), ('ne', xa != xb), ('lt', xa < xb), ('le', xa <= xb), ('gt', xa > xb), ('ge', xa >= xb)]:
                    t.w('setp.%s.%s %%p, %s, %s;' % (cn, ty, ra, rb)); t.outp(int(f))
                    t.w('setp.%su.%s %%p, %s, %s;' % (cn, ty, ra, rb)); t.outp(int(f))
                t.w('setp.num.%s %%p, %s, %s;' % (ty, ra, rb)); t.outp(1)
                t.w('setp.nan.%s %%p, %s, %s;' % (ty, ra, rb)); t.outp(0)
                for c in vals[:6] + vals[12:16]:
                    t.w(('mov.%s %s, ' + lit + ';') % (ty, rc, c))
                    for mode in ['rn', 'rz', 'rm', 'rp']:
                        t.w('fma.%s.%s %s, %s, %s, %s;' % (mode, ty, rx, ra, rb, rc)); out(rx, arith(fmt, 'fma', mode, a, b, c))
            t.w(('mov.%s %s, ' + lit + ';') % (ty, ra, a))
            for mode in ['rn', 'rz', 'rm', 'rp']:
                r = arith(fmt, 'sqrt', mode, a, 0)
                if r is not None:
                    t.w('sqrt.%s.%s %s, %s;' % (mode, ty, rx, ra)); out(rx, r)
                r = arith(fmt, 'rcp', mode, a, 0)
                if r is not None:
                    t.w('rcp.%s.%s %s, %s;' % (mode, ty, rx, ra)); out(rx, r)
            t.w('abs.%s %s, %s;' % (ty, rx, ra)); out(rx, a & ~(1 << (fmt.bits - 1)))
            t.w('neg.%s %s, %s;' % (ty, rx, ra)); out(rx, a ^ (1 << (fmt.bits - 1)))
            (_, sa, xa) = fmt.dec(a)
            sub = xa != 0 and abs(xa) < Fraction(2) ** (1 - fmt.bias)
            for (cn, f) in [('finite', 1), ('infinite', 0), ('number', 1), ('notanumber', 0), ('normal', int(not sub)), ('subnormal', int(sub))]:
                t.w('testp.%s.%s %%p, %s;' % (cn, ty, ra)); t.outp(f)
        # the special values
        inf = fmt.inf(0); ninf = fmt.inf(1); nan = fmt.nan(); one = fmt.enc(Fraction(1)); 
        for a in [inf, ninf, nan]:
            t.cut()
            t.w(('mov.%s %s, ' + lit + ';') % (ty, ra, a))
            for (cn, f) in [('finite', 0), ('infinite', int(a != nan)), ('number', int(a != nan)), ('notanumber', int(a == nan)), ('normal', 0), ('subnormal', 0)]:
                t.w('testp.%s.%s %%p, %s;' % (cn, ty, ra)); t.outp(f)
            for b in [one, inf, nan]:
                t.w(('mov.%s %s, ' + lit + ';') % (ty, rb, b))
                un = a == nan or b == nan
                def val(x): return {inf: 2, ninf: -2, one: 1}.get(x, 0)
                xa = val(a); xb = val(b)
                for (cn, f) in [('eq', xa == xb), ('ne', xa != xb), ('lt', xa < xb), ('le', xa <= xb), ('gt', xa > xb), ('ge', xa >= xb)]:
                    t.w('setp.%s.%s %%p, %s, %s;' % (cn, ty, ra, rb)); t.outp(int(f and not un))
                    t.w('setp.%su.%s %%p, %s, %s;' % (cn, ty, ra, rb)); t.outp(int(f or un))
                t.w('setp.num.%s %%p, %s, %s;' % (ty, ra, rb)); t.outp(int(not un))
                t.w('setp.nan.%s %%p, %s, %s;' % (ty, ra, rb)); t.outp(int(un))
                # min and max give the operand that is a number
                if (a == nan) != (b == nan):
                    o = b if a == nan else a
                    t.w('min.%s %s, %s, %s;' % (ty, rx, ra, rb)); out(rx, o)
                    t.w('max.%s %s, %s, %s;' % (ty, rx, rb, ra)); out(rx, o)
                elif not un:
                    t.w('min.%s %s, %s, %s;' % (ty, rx, ra, rb)); out(rx, a if xa <= xb else b)
                    t.w('add.%s %s, %s, %s;' % (ty, rx, ra, rb))
                    if not (xa == -2 and xb == 2): out(rx, a if abs(xa) == 2 else b)
                    else: t.w('setp.nan.%s %%p, %s, %s;' % (ty, rx, rx)); t.outp(1)
        t.finish(d)

# What is not a number: the manual says 0, and that is what the hardware gives from a float to 32 bits or
# fewer; from a double, or to 64 bits, it gives the top bit of the type.
def to_int(kind, sign, x, mode, bits, signed, dbl=0):
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    if kind == 'nan': return 1 << (bits - 1) if dbl or bits == 64 else 0
    if kind == 'inf': return (lo if sign else hi) & ((1 << bits) - 1)
    fl = x.numerator // x.denominator; r = x - fl
    if mode == 'rni': q = fl + (1 if (r > Fraction(1, 2) or (r == Fraction(1, 2) and fl & 1)) else 0)
    elif mode == 'rzi': q = fl if x >= 0 or r == 0 else fl + 1
    elif mode == 'rmi': q = fl
    else: q = fl if r == 0 else fl + 1
    return max(lo, min(hi, q)) & ((1 << bits) - 1)

# The hardware's assembler refuses .sat where the destination holds every value of the source.
def sat_ok(lo, hi, sbits, ssigned):
    slo, shi = (-(1 << (sbits - 1)), (1 << (sbits - 1)) - 1) if ssigned else (0, (1 << sbits) - 1)
    return lo > slo or hi < shi

def gen_cvt(d):
    t = T('cvt')
    special32 = [F32.inf(0), F32.inf(1), F32.nan()]
    fv = [0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 3.7, -3.7, 1e10, -1e10, 3e9, 1e20, -1e20, 2147483520.0, 0.0, -0.0, 4294967040.0, 0.49999997,
          8388609.5, 1e-10, -1e-10, 255.5, 127.5, -128.5, 65535.5, 9.3e18, 1.9e19]
    for a in [f32b(v) for v in fv] + special32:
        t.cut()
        (k, sg, x) = F32.dec(a)
        t.w('mov.f32 %%fa, 0f%08X;' % a)
        for mode in ['rni', 'rzi', 'rmi', 'rpi']:
            t.w('cvt.%s.s32.f32 %%x, %%fa;' % mode); t.out32('%x', to_int(k, sg, x, mode, 32, 1))
            t.w('cvt.%s.u32.f32 %%x, %%fa;' % mode); t.out32('%x', to_int(k, sg, x, mode, 32, 0))
            t.w('cvt.%s.s64.f32 %%lx, %%fa;' % mode); t.out64('%lx', to_int(k, sg, x, mode, 64, 1))
            t.w('cvt.%s.u64.f32 %%lx, %%fa;' % mode); t.out64('%lx', to_int(k, sg, x, mode, 64, 0))
            t.w('cvt.%s.s16.f32 %%hx, %%fa;' % mode); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', to_int(k, sg, x, mode, 16, 1))
            t.w('cvt.%s.u8.f32 %%hx, %%fa;' % mode); t.w('cvt.u32.u8 %x, %hx;'); t.out32('%x', to_int(k, sg, x, mode, 8, 0))
            t.w('cvt.%s.s8.f32 %%hx, %%fa;' % mode); t.w('cvt.u32.u8 %x, %hx;'); t.out32('%x', to_int(k, sg, x, mode, 8, 1))
            if k == 'num' and abs(x) < 2 ** 60:
                fl = x.numerator // x.denominator; r = x - fl
                q = to_int(k, sg, x, mode, 128, 1); q = q - (1 << 128) if q >> 127 else q
                zs = sg
                t.w('cvt.%s.f32.f32 %%fx, %%fa;' % mode); t.out32('%fx', F32.enc(Fraction(q), 'rn', zs))
        if k != 'nan':
            t.w('cvt.f64.f32 %da, %fa;')
            t.out64('%da', F64.inf(sg) if k == 'inf' else F64.enc(x, 'rn', sg))
        if k == 'num':
            for mode in ['rn', 'rz', 'rm', 'rp']:
                t.w('cvt.%s.f16.f32 %%hx, %%fa;' % mode); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', F16.enc(x, mode, sg))
            t.w('cvt.rn.bf16.f32 %hx, %fa;'); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', BF16.enc(x, 'rn', sg))
            t.w('cvt.rna.tf32.f32 %x, %fa;')
            r = Fmt(10, 8).enc(x, 'rna', sg) << 13
            t.out32('%x', r)
            if not (x == 0 and sg): t.w('cvt.sat.f32.f32 %fx, %fa;'); t.out32('%fx', a if 0 <= x <= 1 and not (x == 0 and sg) else (0 if x <= 0 else 0x3f800000))
    dv = [0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 3.7, -3.7, 1e10, -1e10, 3e9, 1e20, -1e20, 2147483647.5, 2147483648.5, -2147483648.5, 0.0, -0.0,
          4294967295.5, 0.49999999999999994, 1e-10, 9.3e18, 1.9e19, 1e39, -1e39, 1e-46, 1.0000000596046448, 3.4028235677973366e38,
          65504.0, 65519.99, 65520.0, 1e-8, 6e-8, 2.98e-8, 2.9802322387695312e-08, 0.1, 1 / 3.0]
    for a in [f64b(v) for v in dv] + [F64.inf(0), F64.inf(1), F64.nan()]:
        t.cut()
        (k, sg, x) = F64.dec(a)
        t.w('mov.f64 %%da, 0d%016X;' % a)
        for mode in ['rni', 'rzi', 'rmi', 'rpi']:
            t.w('cvt.%s.s32.f64 %%x, %%da;' % mode); t.out32('%x', to_int(k, sg, x, mode, 32, 1, 1))
            t.w('cvt.%s.u32.f64 %%x, %%da;' % mode); t.out32('%x', to_int(k, sg, x, mode, 32, 0, 1))
            t.w('cvt.%s.s64.f64 %%lx, %%da;' % mode); t.out64('%lx', to_int(k, sg, x, mode, 64, 1, 1))
            t.w('cvt.%s.u64.f64 %%lx, %%da;' % mode); t.out64('%lx', to_int(k, sg, x, mode, 64, 0, 1))
            t.w('cvt.%s.s16.f64 %%hx, %%da;' % mode); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', to_int(k, sg, x, mode, 16, 1, 1))
            t.w('cvt.%s.u8.f64 %%hx, %%da;' % mode); t.w('cvt.u32.u8 %x, %hx;'); t.out32('%x', to_int(k, sg, x, mode, 8, 0, 1))
            t.w('cvt.%s.s8.f64 %%hx, %%da;' % mode); t.w('cvt.u32.u8 %x, %hx;'); t.out32('%x', to_int(k, sg, x, mode, 8, 1, 1))
            if k == 'num' and abs(x) < 2 ** 60:
                q = to_int(k, sg, x, mode, 128, 1); q = q - (1 << 128) if q >> 127 else q
                t.w('cvt.%s.f64.f64 %%dx, %%da;' % mode); t.out64('%dx', F64.enc(Fraction(q), 'rn', sg))
        if k == 'num':
            for mode in ['rn', 'rz', 'rm', 'rp']:
                t.w('cvt.%s.f32.f64 %%fx, %%da;' % mode); t.out32('%fx', F32.enc(x, mode, sg))
                t.w('cvt.%s.f16.f64 %%hx, %%da;' % mode); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', F16.enc(x, mode, sg))
        elif k == 'inf':
            t.w('cvt.rn.f32.f64 %fx, %da;'); t.out32('%fx', F32.inf(sg))
            t.w('cvt.rn.f16.f64 %hx, %da;'); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', F16.inf(sg))
    # every half-precision number, back to single and double
    for h in list(range(0, 0x10000, 97)) + [0x7c00, 0xfc00, 0x0001, 0x8001, 0x03ff, 0x0400, 0x7bff, 0xfbff]:
        t.cut()
        (k, sg, x) = F16.dec(h)
        if k == 'nan': continue
        t.w('mov.b16 %%ha, 0x%x;' % h)
        t.w('cvt.f32.f16 %fx, %ha;'); t.out32('%fx', F32.inf(sg) if k == 'inf' else F32.enc(x, 'rn', sg))
        t.w('cvt.f64.f16 %dx, %ha;'); t.out64('%dx', F64.inf(sg) if k == 'inf' else F64.enc(x, 'rn', sg))
        (k, sg, x) = BF16.dec(h)
        if k == 'num':
            t.w('cvt.f32.bf16 %fx, %ha;'); t.out32('%fx', h << 16)
    # integers to floating point
    iv = [0, 1, 16777216, 16777217, 16777219, 0x7fffffff, 0x80000000, 0xffffffff, 0xffffff7f, 0xffffff80, 0xffffff81, 0x80000081, 33554434, 33554438]
    for a in iv:
        t.cut()
        t.w('mov.b32 %%a, 0x%x;' % a)
        for mode in ['rn', 'rz', 'rm', 'rp']:
            t.w('cvt.%s.f32.s32 %%fx, %%a;' % mode); t.out32('%fx', F32.enc(Fraction(s32(a)), mode))
            t.w('cvt.%s.f32.u32 %%fx, %%a;' % mode); t.out32('%fx', F32.enc(Fraction(a), mode))
        t.w('cvt.rn.f64.s32 %dx, %a;'); t.out64('%dx', F64.enc(Fraction(s32(a))))
        t.w('cvt.rn.f64.u32 %dx, %a;'); t.out64('%dx', F64.enc(Fraction(a)))
        t.w('cvt.rn.f16.s32 %hx, %a;'); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', F16.enc(Fraction(s32(a))))
    lv = [0, 1, M64, 0x7fffffffffffffff, 0x8000000000000000, 0x8000008000000000, 0x8000018000000000, 0xfffffffffffffbff, 0xfffffffffffffc00,
          0x20000000000001, 0x20000000000003, 0xffffff8000000000, 0x7fffffbfffffffff, 0x7fffffc000000000, 0x1000001, 0x400000040000001]
    for a in lv:
        t.cut()
        t.w('mov.b64 %%la, 0x%x;' % a)
        for mode in ['rn', 'rz', 'rm', 'rp']:
            t.w('cvt.%s.f32.s64 %%fx, %%la;' % mode); t.out32('%fx', F32.enc(Fraction(s64(a)), mode))
            t.w('cvt.%s.f32.u64 %%fx, %%la;' % mode); t.out32('%fx', F32.enc(Fraction(a), mode))
            t.w('cvt.%s.f64.s64 %%dx, %%la;' % mode); t.out64('%dx', F64.enc(Fraction(s64(a)), mode))
            t.w('cvt.%s.f64.u64 %%dx, %%la;' % mode); t.out64('%dx', F64.enc(Fraction(a), mode))
    # integers to integers
    for a in [0, 1, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x12345678, 0x7fffffff, 0x80000000, 0xffffffff, 0xffffff80, 0xffff8000, 0xfffffe00]:
        t.cut()
        t.w('mov.b32 %%a, 0x%x;' % a)
        for (dt, db, ds) in [('s8', 8, 1), ('u8', 8, 0), ('s16', 16, 1), ('u16', 16, 0), ('s32', 32, 1), ('u32', 32, 0), ('s64', 64, 1), ('u64', 64, 0)]:
            for (st, ss) in [('s32', 1), ('u32', 0)]:
                v = s32(a) if ss else a
                if db == 64:
                    t.w('cvt.%s.%s %%lx, %%a;' % (dt, st)); t.out64('%lx', v & M64)
                    lo, hi = (-(1 << 63), (1 << 63) - 1) if ds else (0, M64)
                    if sat_ok(lo, hi, 32, ss): t.w('cvt.sat.%s.%s %%lx, %%a;' % (dt, st)); t.out64('%lx', max(lo, min(hi, v)) & M64)
                else:
                    lo, hi = (-(1 << (db - 1)), (1 << (db - 1)) - 1) if ds else (0, (1 << db) - 1)
                    if db == 32:
                        t.w('cvt.%s.%s %%x, %%a;' % (dt, st)); t.out32('%x', v)
                        if sat_ok(lo, hi, 32, ss): t.w('cvt.sat.%s.%s %%x, %%a;' % (dt, st)); t.out32('%x', max(lo, min(hi, v)))
                    else:
                        # what a narrow result looks like in a wider register is seen through a store of its size
                        t.w('cvt.%s.%s %%x, %%a;' % (dt, st)); t.w('and.b32 %%x, %%x, 0x%x;' % ((1 << db) - 1)); t.out32('%x', v & ((1 << db) - 1))
                        t.w('cvt.sat.%s.%s %%x, %%a;' % (dt, st)); t.w('and.b32 %%x, %%x, 0x%x;' % ((1 << db) - 1))
                        t.out32('%x', max(lo, min(hi, v)) & ((1 << db) - 1))
        for (st, sb, ss) in [('s8', 8, 1), ('u8', 8, 0), ('s16', 16, 1), ('u16', 16, 0)]:
            v = sx(a, sb) if ss else a & ((1 << sb) - 1)
            t.w('cvt.s32.%s %%x, %%a;' % st); t.out32('%x', v)
            t.w('cvt.u64.%s %%lx, %%a;' % st); t.out64('%lx', v & M64)
    for a in [0, 1, M64, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000, 0xffffffff80000000, 0xffffffff7fffffff, 0x8000000000000000, 0x7fffffffffffffff]:
        t.cut()
        t.w('mov.b64 %%la, 0x%x;' % a)
        for (dt, db, ds) in [('s32', 32, 1), ('u32', 32, 0), ('s64', 64, 1), ('u64', 64, 0)]:
            for (st, ss) in [('s64', 1), ('u64', 0)]:
                v = s64(a) if ss else a
                lo, hi = (-(1 << (db - 1)), (1 << (db - 1)) - 1) if ds else (0, (1 << db) - 1)
                if db == 64:
                    t.w('cvt.%s.%s %%lx, %%la;' % (dt, st)); t.out64('%lx', v & M64)
                    if sat_ok(lo, hi, 64, ss): t.w('cvt.sat.%s.%s %%lx, %%la;' % (dt, st)); t.out64('%lx', max(lo, min(hi, v)) & M64)
                else:
                    t.w('cvt.%s.%s %%x, %%la;' % (dt, st)); t.out32('%x', v)
                    t.w('cvt.sat.%s.%s %%x, %%la;' % (dt, st)); t.out32('%x', max(lo, min(hi, v)))
    t.finish(d)

    # the 8-bit formats, two at a time
    t = T('fp8', 'sm_120a', '9.1')
    vals = [0.0, 1.0, -1.0, 0.5, 448.0, 464.0, 465.0, 480.0, 1e9, -1e9, 0.001953125, 0.0009765625, 0.0029296875, 1.0625, 1.1875, 3.3, -7.77,
            57344.0, 61440.0, 61441.0, 1.52587890625e-05, 7.62939453125e-06, 2.2888e-05, 100.0, 0.3, 26.0, 27.0, 0.015625, -0.0146484375]
    for i in range(0, len(vals) - 1):
        t.cut()
        a = f32b(vals[i]); b = f32b(vals[i + 1])
        (_, sa, xa) = F32.dec(a); (_, sb, xb) = F32.dec(b)
        t.w('mov.f32 %%fa, 0f%08X; mov.f32 %%fb, 0f%08X;' % (a, b))
        # the first source lands in the upper byte
        t.w('cvt.rn.satfinite.e4m3x2.f32 %hx, %fa, %fb;'); t.w('cvt.u32.u16 %x, %hx;')
        t.out32('%x', E4M3.enc(xa, 'rn', sa, True) << 8 | E4M3.enc(xb, 'rn', sb, True))
        t.w('cvt.rn.satfinite.e5m2x2.f32 %hx, %fa, %fb;'); t.w('cvt.u32.u16 %x, %hx;')
        t.out32('%x', E5M2.enc(xa, 'rn', sa, True) << 8 | E5M2.enc(xb, 'rn', sb, True))
    # the 4-bit format as ufsm writes it, through a .b8 register of a block of its own
    v4 = [0.0, 0.25, 0.2500001, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 2.4999, 3.0, 3.5, 4.0, 5.0, 5.0001, 6.0, 7.0, 1e9, -0.0, -0.25,
          -0.75, -1.25, -2.5, -3.5, -5.0, -9.0, 0.1, -0.3]
    for i in range(0, len(v4) - 1):
        t.cut()
        a = f32b(v4[i]); b = f32b(v4[i + 1])
        (_, sa, xa) = F32.dec(a); (_, sb, xb) = F32.dec(b)
        t.w('mov.f32 %%fa, 0f%08X; mov.f32 %%fb, 0f%08X;' % (a, b))
        t.w('{ .reg .b8 t; cvt.rn.satfinite.e2m1x2.f32 t, %fa, %fb; cvt.u16.u8 %hx, t; }'); t.w('cvt.u32.u16 %x, %hx;')
        t.out32('%x', E2M1.enc(xa, 'rn', sa, True) << 4 | E2M1.enc(xb, 'rn', sb, True))
    for v in range(256):
        t.cut()
        r = 0
        for (i, e) in enumerate([v & 15, v >> 4]):
            (k, sg, x) = E2M1.dec(e)
            r |= F16.enc(x, 'rn', sg) << 16 * i
        t.w('mov.b32 %%a, 0x%x;' % (v | 0x5500))
        t.w('{ .reg .b8 t; cvt.u8.u32 t, %a; cvt.rn.f16x2.e2m1x2 %x, t; }'); t.out32('%x', r)
    # what is not a number, and the infinities (as ufsm's software versions document the hardware)
    t.w('mov.f32 %fa, 0f7FC00000; mov.f32 %fb, 0f7F800000; mov.f32 %fc, 0fFF800000;')
    t.w('cvt.rn.satfinite.e4m3x2.f32 %hx, %fa, %fb;'); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', 0x7f7e)
    t.w('cvt.rn.satfinite.e4m3x2.f32 %hx, %fc, %fa;'); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', 0xfe7f)
    t.w('{ .reg .b8 t; cvt.rn.satfinite.e2m1x2.f32 t, %fa, %fb; cvt.u16.u8 %hx, t; }'); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', 0x77)
    t.w('{ .reg .b8 t; cvt.rn.satfinite.e2m1x2.f32 t, %fc, %fa; cvt.u16.u8 %hx, t; }'); t.w('cvt.u32.u16 %x, %hx;'); t.out32('%x', 0xf7)
    for v in range(0, 0x10000, 257 * 3 + 1):
        t.cut()
        hi = v >> 8; lo = v & 0xff
        t.w('mov.b16 %%ha, 0x%x;' % v)
        ok = True; r = 0
        for (i, e) in enumerate([lo, hi]):
            (k, sg, x) = E4M3.dec(e)
            if k != 'num': ok = False
            else: r |= F16.enc(x, 'rn', sg) << 16 * i
        if ok:
            t.w('cvt.rn.f16x2.e4m3x2 %x, %ha;'); t.out32('%x', r)
        ok = True; r = 0
        for (i, e) in enumerate([lo, hi]):
            (k, sg, x) = E5M2.dec(e)
            if k == 'nan': ok = False
            elif k == 'inf': r |= F16.inf(sg) << 16 * i
            else: r |= F16.enc(x, 'rn', sg) << 16 * i
        if ok:
            t.w('cvt.rn.f16x2.e5m2x2 %x, %ha;'); t.out32('%x', r)
    t.finish(d)

# ---------------- mma, ldmatrix, cp.async ----------------

class Rng:
    def __init__(self, seed): self.s = seed
    def next(self, lo, hi):
        self.s = (self.s * 6364136223846793005 + 1442695040888963407) & M64
        return lo + (self.s >> 33) % (hi - lo + 1)

def enc_elem(kind, v):
    if kind == 'f16': return F16.enc(Fraction(v))
    if kind == 'bf16': return BF16.enc(Fraction(v))
    if kind == 'tf32': return F32.enc(Fraction(v))
    if kind == 'e4m3': return E4M3.enc(Fraction(v))
    if kind == 'e5m2': return E5M2.enc(Fraction(v))
    return v & 0xff

def gen_mma(d):
    # (name, the instruction, K, element kind, bits of an element, registers of A, of B, the accumulator, scale of the values)
    shapes = [('k16f16', 'mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32', 16, 'f16', 16, 4, 2, 'f32', 4),
              ('k16bf16', 'mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32', 16, 'bf16', 16, 4, 2, 'f32', 2),
              ('k8f16', 'mma.sync.aligned.m16n8k8.row.col.f32.f16.f16.f32', 8, 'f16', 16, 2, 1, 'f32', 8),
              ('k8tf32', 'mma.sync.aligned.m16n8k8.row.col.f32.tf32.tf32.f32', 8, 'tf32', 32, 4, 2, 'f32', 16),
              ('k16h', 'mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16', 16, 'f16', 16, 4, 2, 'f16', 1),
              ('k8h', 'mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16', 8, 'f16', 16, 2, 1, 'f16', 1),
              ('k32s8', 'mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32', 32, 's8', 8, 4, 2, 's32', 1),
              ('k32us8', 'mma.sync.aligned.m16n8k32.row.col.s32.u8.s8.s32', 32, 'u8s8', 8, 4, 2, 's32', 1),
              ('k16s8', 'mma.sync.aligned.m16n8k16.row.col.s32.s8.u8.s32', 16, 's8u8', 8, 2, 1, 's32', 1),
              ('k32e4m3', 'mma.sync.aligned.m16n8k32.row.col.f32.e4m3.e4m3.f32', 32, 'e4m3', 8, 4, 2, 'f32', 4),
              ('k32e5m2', 'mma.sync.aligned.m16n8k32.row.col.f32.e5m2.e5m2.f32', 32, 'e5m2', 8, 4, 2, 'f32', 2),
              ('k32sat', 'mma.sync.aligned.m16n8k32.row.col.satfinite.s32.s8.s8.s32', 32, 's8', 8, 4, 2, 's32sat', 1)]
    ptx = []; exp = []; runs = []
    # the block-scaled forms need this version and target; the others are as old as sm_80
    ptx.append('.version 9.1\n.target sm_120a\n.address_size 64\n')
    rng = Rng(12345)
    for (name, ins, K, kind, ebits, na, nb, acc, scale) in shapes:
        ka = kb = kind
        if kind == 'u8s8': ka, kb = 'u8', 's8'
        if kind == 's8u8': ka, kb = 's8', 'u8'
        isint = acc.startswith('s32')
        def val(k):
            if k == 's8': return rng.next(-128, 127)
            if k == 'u8': return rng.next(0, 255)
            return Fraction(rng.next(-3 * scale, 3 * scale), scale)
        A = [[val(ka) for c in range(K)] for r in range(16)]
        B = [[val(kb) for c in range(8)] for r in range(K)]
        if acc == 's32sat': C = [[rng.next(-2**31, 2**31 - 1) if (r + c) % 3 else rng.next(-1000, 1000) for c in range(8)] for r in range(16)]
        elif isint: C = [[rng.next(-2**31, 2**31 - 1) for c in range(8)] for r in range(16)]
        else: C = [[Fraction(rng.next(-64, 64), scale) for c in range(8)] for r in range(16)]
        D = [[sum(A[i][k] * B[k][j] for k in range(K)) + C[i][j] for j in range(8)] for i in range(16)]
        epr = 32 // ebits
        nc = 2 if acc == 'f16' else 4
        tab = []
        for lane in range(32):
            g = lane >> 2; t = lane & 3
            for r in range(na):
                w = 0
                for i in range(epr):
                    row = g + 8 * (r & 1); col = epr * t + i + 4 * epr * (r >> 1)
                    w |= enc_elem(ka, A[row][col]) << (ebits * i)
                tab.append(w)
            for r in range(nb):
                w = 0
                for i in range(epr):
                    row = epr * t + i + 4 * epr * r; col = g
                    w |= enc_elem(kb, B[row][col]) << (ebits * i)
                tab.append(w)
            for i in range(4):
                row = g + 8 * (i >> 1); col = 2 * t + (i & 1)
                x = C[row][col]; y = D[row][col]
                if isint:
                    cw = x & M32
                    dw = (max(-2**31, min(2**31 - 1, y)) if acc == 's32sat' else y) & M32
                elif acc == 'f16': cw = F16.enc(x); dw = F16.enc(y)
                else: cw = F32.enc(x); dw = F32.enc(y)
                if acc == 'f16':
                    if i & 1:
                        tab[-1] |= cw << 16; exp[-1] |= dw << 16
                    else:
                        tab.append(cw); exp.append(dw)
                else:
                    tab.append(cw); exp.append(dw)
        per = na + nb + nc
        ptx.append('.global .align 4 .u32 tab_%s[%d] = {%s};\n' % (name, len(tab), ', '.join('0x%x' % v for v in tab)))
        areg = 'b32'
        creg = 'f32' if acc == 'f32' else 'b32'
        L = ['.visible .entry %s(.param .u64 out)\n{' % name, '    .reg .b32 %me;', '    .reg .b64 %ro, %ri, %off;',
             '    .reg .%s %%a<4>, %%b<2>;' % areg, '    .reg .%s %%c<4>, %%d<4>;' % creg,
             '    ld.param.u64 %ro, [out];', '    mov.u32 %me, %tid.x;',
             '    mul.wide.u32 %%off, %%me, %d;' % (4 * per), '    mov.u64 %%ri, tab_%s;' % name, '    add.u64 %ri, %ri, %off;',
             '    mul.wide.u32 %%off, %%me, %d;' % (4 * nc), '    add.u64 %ro, %ro, %off;']
        o = 0
        for r in range(na): L.append('    ld.global.%s %%a%d, [%%ri+%d];' % (areg, r, o)); o += 4
        for r in range(nb): L.append('    ld.global.%s %%b%d, [%%ri+%d];' % (areg, r, o)); o += 4
        for r in range(nc): L.append('    ld.global.%s %%c%d, [%%ri+%d];' % (creg, r, o)); o += 4
        def lst(x, n): return '{' + ', '.join('%%%s%d' % (x, i) for i in range(n)) + '}'
        L.append('    %s %s, %s, %s, %s;' % (ins, lst('d', nc), lst('a', na), lst('b', nb), lst('c', nc)))
        for r in range(nc): L.append('    st.global.%s [%%ro+%d], %%d%d;' % (creg, 4 * r, r))
        L.append('    ret;\n}\n')
        ptx.append('\n'.join(L))
        runs.append((name, 32 * nc))
    # The block-scaled kinds (sm_120a; ufsm does not reach them when built for sm_86). A scale is a power of two, 2^(byte - 127);
    # where the bytes lie is taken from ufsm's software version of the instruction, and has not been compared with hardware:
    # the scale of row i of A for chunk c of K is byte (byte-id + c) of the scale register of lane 4 (i mod 8) + 2 thread-id + i / 8,
    # that of column j of B is byte (byte-id + c) of the scale register of lane 4 j + thread-id.
    for (name, kindname, K, kind, ebits, nchunk, ba, ta, bb, tb) in [
            ('bs1x', 'kind::mxf8f6f4.block_scale.scale_vec::1X.f32.e4m3.e4m3.f32.ue8m0', 32, 'e4m3', 8, 1, 0, 0, 0, 0),
            ('bs2x', 'kind::mxf4.block_scale.scale_vec::2X.f32.e2m1.e2m1.f32.ue8m0', 64, 'e2m1', 4, 2, 0, 0, 0, 0),
            ('bs2xb', 'kind::mxf4.block_scale.scale_vec::2X.f32.e2m1.e2m1.f32.ue8m0', 64, 'e2m1', 4, 2, 2, 1, 2, 3),
            ('bs4x', 'kind::mxf4nvf4.block_scale.scale_vec::4X.f32.e2m1.e2m1.f32.ue8m0', 64, 'e2m1', 4, 4, 0, 0, 0, 0)]:
        e2 = [Fraction(v, 2) for v in [0, 1, 2, 3, 4, 6, 8, 12, -1, -2, -3, -4, -6, -8, -12]]
        def val():
            if kind == 'e2m1': return e2[rng.next(0, len(e2) - 1)]
            return Fraction(rng.next(-12, 12), 4)
        def enc(v): return E2M1.enc(v) if kind == 'e2m1' else E4M3.enc(v)
        A = [[val() for c in range(K)] for r in range(16)]
        B = [[val() for c in range(8)] for r in range(K)]
        C = [[Fraction(rng.next(-64, 64), 4) for c in range(8)] for r in range(16)]
        SA = [sum(rng.next(125, 129) << 8 * i for i in range(4)) for lane in range(32)]
        SB = [sum(rng.next(125, 129) << 8 * i for i in range(4)) for lane in range(32)]
        ch = K // nchunk
        def sca(i, c): return Fraction(2) ** ((SA[4 * (i % 8) + 2 * ta + i // 8] >> 8 * ((ba + c) & 3) & 0xff) - 127)
        def scb(c, j): return Fraction(2) ** ((SB[4 * j + tb] >> 8 * ((bb + c) & 3) & 0xff) - 127)
        D = [[sum(sum(A[i][k] * B[k][j] for k in range(c * ch, (c + 1) * ch)) * sca(i, c) * scb(c, j) for c in range(nchunk)) + C[i][j]
              for j in range(8)] for i in range(16)]
        epr = 32 // ebits
        tab = []
        for lane in range(32):
            g = lane >> 2; t = lane & 3
            for r in range(4):
                tab.append(sum(enc(A[g + 8 * (r & 1)][epr * t + i + 4 * epr * (r >> 1)]) << (ebits * i) for i in range(epr)))
            for r in range(2):
                tab.append(sum(enc(B[epr * t + i + 4 * epr * r][g]) << (ebits * i) for i in range(epr)))
            for i in range(4):
                tab.append(F32.enc(C[g + 8 * (i >> 1)][2 * t + (i & 1)])); exp.append(F32.enc(D[g + 8 * (i >> 1)][2 * t + (i & 1)]))
            tab.append(SA[lane]); tab.append(SB[lane])
        ptx.append('.global .align 4 .u32 tab_%s[%d] = {%s};\n' % (name, len(tab), ', '.join('0x%x' % v for v in tab)))
        L = ['.visible .entry %s(.param .u64 out)\n{' % name, '    .reg .b32 %me, %sa, %sb;', '    .reg .b64 %ro, %ri, %off;',
             '    .reg .b32 %a<4>, %b<2>;', '    .reg .f32 %c<4>;',
             '    ld.param.u64 %ro, [out];', '    mov.u32 %me, %tid.x;',
             '    mul.wide.u32 %off, %me, 48;', '    mov.u64 %%ri, tab_%s;' % name, '    add.u64 %ri, %ri, %off;',
             '    mul.wide.u32 %off, %me, 16;', '    add.u64 %ro, %ro, %off;']
        for r in range(4): L.append('    ld.global.u32 %%a%d, [%%ri+%d];' % (r, 4 * r))
        for r in range(2): L.append('    ld.global.u32 %%b%d, [%%ri+%d];' % (r, 16 + 4 * r))
        for r in range(4): L.append('    ld.global.f32 %%c%d, [%%ri+%d];' % (r, 24 + 4 * r))
        L.append('    ld.global.u32 %sa, [%ri+40];'); L.append('    ld.global.u32 %sb, [%ri+44];')
        L.append('    mma.sync.aligned.m16n8k%d.row.col.%s {%%c0,%%c1,%%c2,%%c3}, {%%a0,%%a1,%%a2,%%a3}, {%%b0,%%b1}, {%%c0,%%c1,%%c2,%%c3}, %%sa, {%d, %d}, %%sb, {%d, %d};'
                 % (K, kindname, ba, ta, bb, tb))
        for r in range(4): L.append('    st.global.f32 [%%ro+%d], %%c%d;' % (4 * r, r))
        L.append('    ret;\n}\n')
        ptx.append('\n'.join(L))
        runs.append((name, 128))
    f = open('%s/g_mma.ptx' % d, 'w')
    f.write('// made by gen.py: D = A B + C for the shapes of mma.sync ufsm uses, the fragments packed as the PTX manual lays them out\n')
    f.write(''.join('//! run %s 1 1 1 32 1 1 0 %d\n' % r for r in runs))
    f.write('\n'.join(ptx))
    f.close()
    f = open('%s/g_mma.exp' % d, 'w')
    k = 0
    for (name, n) in runs:
        f.write('%s: 0\n' % name)
        for i in range(n):
            f.write('%08x%s' % (exp[k + i], '\n' if i % 8 == 7 or i == n - 1 else ' '))
        k += n
    f.close()

    # ldmatrix, the matrices brought into shared memory by cp.async
    def elem(i): return (i * 0x9e5 + 0x1234) & 0xffff
    exp = []; runs = []; ptx = []
    ptx.append('.version 8.5\n.target sm_86\n.address_size 64\n')
    ptx.append('.global .align 16 .u16 src[256] = {%s};\n.shared .align 16 .b16 mat[256];\n' % ', '.join('0x%x' % elem(i) for i in range(256)))
    for (name, num, trans, a32, partial) in [('x4', 4, 0, 0, 0), ('x4t', 4, 1, 1, 0), ('x2', 2, 0, 1, 0), ('x1t', 1, 1, 0, 0), ('x1', 1, 0, 0, 0), ('x4p', 4, 0, 0, 1)]:
        L = ['.visible .entry %s(.param .u64 out)\n{' % name, '    .reg .b32 %me, %r<4>, %sa, %n;', '    .reg .b64 %ro, %off, %s, %g, %sg;',
             '    ld.param.u64 %ro, [out];', '    mov.u32 %me, %tid.x;', '    mul.wide.u32 %off, %me, 16;',
             '    mov.u64 %s, mat;', '    add.u64 %s, %s, %off;', '    mov.u64 %g, src;', '    add.u64 %g, %g, %off;']
        if partial:
            # only the first 10 bytes of a row are there to copy: the rest becomes zero
            L += ['    mov.u32 %n, 10;', '    cp.async.ca.shared.global [%s], [%g], 16, %n;']
        else:
            L += ['    cp.async.cg.shared.global.L2::128B [%s], [%g], 16;']
        L += ['    cp.async.commit_group;', '    cp.async.wait_group 0;', '    bar.sync 0;']
        regs = '{' + ', '.join('%%r%d' % i for i in range(num)) + '}' 
        tr = '.trans' if trans else ''
        if a32:
            L += ['    cvt.u32.u64 %sa, %s;', '    ldmatrix.sync.aligned.m8n8.x%d%s.shared.b16 %s, [%%sa];' % (num, tr, regs)]
        else:
            L += ['    ldmatrix.sync.aligned.m8n8.x%d%s.shared.b16 %s, [%%s];' % (num, tr, regs)]
        L += ['    mul.wide.u32 %%off, %%me, %d;' % (4 * num), '    add.u64 %ro, %ro, %off;']
        for i in range(num): L.append('    st.global.u32 [%%ro+%d], %%r%d;' % (4 * i, i))
        L.append('    ret;\n}\n')
        ptx.append('\n'.join(L))
        runs.append((name, 32 * num))
        def m(mi, row, col):
            if partial and col >= 5: return 0
            return elem(mi * 64 + row * 8 + col)
        for lane in range(32):
            for mi in range(num):
                if trans: v = m(mi, 2 * (lane % 4), lane // 4) | m(mi, 2 * (lane % 4) + 1, lane // 4) << 16
                else: v = m(mi, lane // 4, 2 * (lane % 4)) | m(mi, lane // 4, 2 * (lane % 4) + 1) << 16
                exp.append(v)
    f = open('%s/g_ldmatrix.ptx' % d, 'w')
    f.write('// made by gen.py: ldmatrix of 8 x 8 matrices of 16-bit elements that cp.async brought into shared memory\n')
    f.write(''.join('//! run %s 1 1 1 32 1 1 0 %d\n' % r for r in runs))
    f.write('\n'.join(ptx))
    f.close()
    f = open('%s/g_ldmatrix.exp' % d, 'w')
    k = 0
    for (name, n) in runs:
        f.write('%s: 0\n' % name)
        for i in range(n):
            f.write('%08x%s' % (exp[k + i], '\n' if i % 8 == 7 or i == n - 1 else ' '))
        k += n
    f.close()

if __name__ == '__main__':
    d = sys.argv[1]
    gen_int(d)
    gen_flt(d)
    gen_cvt(d)
    gen_mma(d)
