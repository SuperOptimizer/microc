#!/usr/bin/env python3
# mmahw.py: the sums of a floating-point mma as the hardware makes them, found by trying models against what an
# RTX PRO 4000 (sm_120) gave for the data sets of tests/cuda/hw/mma.txt (inputs and the device's words):
#   - every product a * b is exact, and its exponent is the sum of the two exponents (not that of its top bit);
#   - E is the largest exponent among c and the products that are not zero;
#   - every term, c too, is cut toward zero to a multiple of 2^(E - 25): a 24-bit significand at E and two bits;
#   - the terms are added exactly, and the sum is cut toward zero to .f32, or rounded to the nearest (even) .f16
#     when the accumulator is .f16;
#   - too large is infinite; below the smallest normal number the same cut or rounding is made at the
#     subnormal's last place; a zero result is +0 whatever the signs; what is not a number is 0x7fffffff
#     (0x7fff in .f16).
# It holds for f16 (k8, k16), bf16, tf32 (the low 13 bits of an operand are dropped), e4m3, e5m2 and the .f16
# accumulators, and for the block-scaled kind::mxf8f6f4 with a product's exponent that of its factors and of
# its two scales together: every word of every set. kind::mxf4 and mxf4nvf4 are made another way: see hwsum4.
# The engine does the same with MCUDA_MMA_HW=1 (cuda/ptxops.c).
#   mmahw.py check           every recorded set against the model
#   mmahw.py ptx NAME DIR    DIR/NAME.ptx and DIR/NAME.exp (the device's words) of a recorded set, for ptxt
#   mmahw.py test DIR        DIR/h_mma.ptx and .exp: all of them as one test, which ptxt runs with the hardware's sums
#   mmahw.py new SHAPE SEED SPREAD CSPREAD OUT.ptx    random data (exponents within SPREAD of 1; CSPREAD 0: c = 0)
#   mmahw.py add NAME FILE.ptx FILE.out               record what the device printed (gpuptx) for FILE.ptx
import os, random, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SHAPES = {  # the kernels of g_mma.ptx: K, kind of element, its bits, registers of A, of B, accumulator
    'k16f16': (16, 'f16', 16, 4, 2, 'f32'), 'k16bf16': (16, 'bf16', 16, 4, 2, 'f32'), 'k8f16': (8, 'f16', 16, 2, 1, 'f32'),
    'k8tf32': (8, 'tf32', 32, 4, 2, 'f32'), 'k16h': (16, 'f16', 16, 4, 2, 'f16'), 'k8h': (8, 'f16', 16, 2, 1, 'f16'),
    'k32e4m3': (32, 'e4m3', 8, 4, 2, 'f32'), 'k32e5m2': (32, 'e5m2', 8, 4, 2, 'f32')}
# the block-scaled kernels: K, kind, bits, registers of A, of B, accumulator, then chunks of K with a scale each and
# byte-id-a, thread-id-a, byte-id-b, thread-id-b as the kernel of g_mma.ptx has them
BS = {'bs1x': (32, 'e4m3', 8, 4, 2, 'f32', 1, 0, 0, 0, 0), 'bs2x': (64, 'e2m1', 4, 4, 2, 'f32', 2, 0, 0, 0, 0),
      'bs2xb': (64, 'e2m1', 4, 4, 2, 'f32', 2, 2, 1, 2, 3), 'bs4x': (64, 'e2m1', 4, 4, 2, 'f32', 4, 0, 0, 0, 0)}
# the kernel bs1x with other kinds of element: its name, the kind of A, of B (e2m1b: e2m1 held in a byte,
# bits 2 to 5 of it; the hardware reads bits 0 to 5 as e2m3, which is the same when bits 0 and 1 are zero)
ALT = {'bs1e5': ('e5m2', 'e5m2'), 'bs1e23': ('e2m3', 'e3m2'), 'bs1e32': ('e3m2', 'e2m3'), 'bs1e14': ('e2m1b', 'e4m3'),
       'bs1e51': ('e5m2', 'e2m1b')}
for k in ALT: BS[k] = (32, ALT[k], 8, 4, 2, 'f32', 1, 0, 0, 0, 0)
for k in BS: SHAPES[k] = BS[k][:6]
FMT = {'f16': (5, 10), 'bf16': (8, 7), 'tf32': (8, 10), 'e4m3': (4, 3), 'e5m2': (5, 2), 'f32': (8, 23), 'e2m1': (2, 1),
       'e2m1b': (2, 3), 'e2m3': (2, 3), 'e3m2': (3, 2)}
SH = 400

def dec(kind, bits):   # sign, mantissa, exponent of the mantissa's unit, 0 or 1 (infinite) or 2 (not a number)
    (eb, mb) = FMT[kind]
    if kind == 'tf32': bits >>= 13
    bits &= (2 << (eb + mb)) - 1
    s = bits >> (eb + mb) & 1; e = bits >> mb & ((1 << eb) - 1); m = bits & ((1 << mb) - 1); bias = (1 << (eb - 1)) - 1
    if kind == 'e4m3':
        if e == 15 and m == 7: return (s, 0, 0, 2)
    elif eb < 4: pass
    elif e == (1 << eb) - 1: return (s, 0, 0, 2 if m else 1)
    if e == 0: return (s, m, 1 - bias - mb, 0)
    return (s, m | 1 << mb, e - bias - mb, 0)

def enc(acc, s, m, e):   # m * 2^e: cut to .f32, rounded to .f16; too large is infinite, too small is subnormal
    (eb, mb) = FMT[acc]; bias = (1 << (eb - 1)) - 1; W = mb + 1
    sh = max(m.bit_length() - W, 1 - bias - mb - e)   # what goes: all below the significand, or below the smallest
    if sh > 0:
        r = m & ((1 << sh) - 1); m >>= sh; e += sh
        if acc == 'f16' and (r > 1 << (sh - 1) or (r == 1 << (sh - 1) and m & 1)): m += 1
        if m >> W: m >>= 1; e += 1
    else: m <<= -sh; e += sh
    while m and not m >> mb and e > 1 - bias - mb: m <<= 1; e -= 1
    be = e + bias + mb
    if not m >> mb: be = 0
    if not m: return 0   # +0 whatever the sign
    if be >= (1 << eb) - 1: return s << (eb + mb) | ((1 << eb) - 1) << mb
    return s << (eb + mb) | be << mb | (m & ((1 << mb) - 1))

def hwsum4(prods, c):
    # mxf4 and mxf4nvf4 (e2m1 in four bits, K = 64, .f32): the products are added sixteen at a time, exactly, and
    # each such sum times its scale is cut to a multiple of 2^(E - 35), as is c; E is the largest of c's exponent
    # and the exponents of the scales of those blocks that have a product that is not zero.
    (sc, mc, ec, xc) = dec('f32', c)
    if xc: return None
    terms = [(-mc if sc else mc, ec)]; es = [ec + 23] if mc else []
    for g in range(0, len(prods), 16):
        I = 0
        for (a, b, se) in prods[g:g + 16]:
            (sa, ma, ea, xa) = dec('e2m1', a); (sb, mb_, eb_, xb) = dec('e2m1', b)
            v = (ma * mb_) << (ea + eb_ + 4)
            I += -v if sa ^ sb else v
            if v: es.append(se)
        terms.append((I, se - 4))
    if not es: return 0
    lsb = max(es) - 35 + SH; S = 0
    for (I, e) in terms:
        v = abs(I) << (e + SH)
        v = v >> lsb if lsb > 0 else v << -lsb
        S += -v if I < 0 else v
    return enc('f32', 1 if S < 0 else 0, abs(S), lsb - SH) if S else 0

def hwsum(kind, acc, prods, c):   # prods: pairs of element bit patterns; c: the accumulator's
    if kind == 'e2m1': return hwsum4(prods, c)
    kb = kind
    if type(kind) is tuple: (kind, kb) = kind
    (eb, mb) = FMT[kind]; (ceb, W) = FMT[acc]; mbb = FMT[kb][1]
    nan = (1 << (ceb + W)) - 1; inf = ((1 << ceb) - 1) << W
    terms = []; es = []; infs = set()
    for pr in prods:   # (a, b) or, block-scaled, (a, b, the exponent of the two scales together)
        (a, b) = pr[:2]
        (sa, ma, ea, xa) = dec(kind, a); (sb, mb_, eb_, xb) = dec(kb, b)
        if len(pr) > 2: ea += pr[2]
        if xa == 2 or xb == 2 or (xa == 1 and not xb and not mb_) or (xb == 1 and not xa and not ma): return nan
        if xa or xb: infs.add(sa ^ sb); continue
        terms.append((sa ^ sb, ma * mb_, ea + eb_))
        if ma * mb_: es.append(mb + mbb + ea + eb_)
    (sc, mc, ec, xc) = dec(acc, c)
    if xc == 2: return nan
    if xc: infs.add(sc)
    if len(infs) == 2: return nan
    if infs: return infs.pop() << (ceb + W) | inf
    terms.append((sc, mc, ec))
    if mc: es.append(W + ec)
    if not es: return 0
    lsb = max(es) - 25 + SH; S = 0
    for (s, m, e) in terms:
        v = m << (e + SH)
        v = v >> lsb if lsb > 0 else v << -lsb
        S += -v if s else v
    return enc(acc, 1 if S < 0 else 0, abs(S), lsb - SH) if S else 0

def layout(shape):
    (K, kind, ebits, na, nb, acc) = SHAPES[shape]
    return K, kind, ebits, na, nb, acc, 32 // ebits, 2 if acc == 'f16' else 4

def products(shape, tab, A, B, i, j):
    K = SHAPES[shape][0]
    if shape not in BS: return [(A[i][k], B[k][j]) for k in range(K)]
    (nch, ba, ta, bb, tb) = BS[shape][6:]
    r = []
    for k in range(K):
        c = k // (K // nch)
        sa = tab[12 * (4 * (i % 8) + 2 * ta + i // 8) + 10] >> 8 * ((ba + c) & 3) & 0xff
        sb = tab[12 * (4 * j + tb) + 11] >> 8 * ((bb + c) & 3) & 0xff
        r.append((A[i][k], B[k][j], sa + sb - 254))
    return r

def matrices(shape, tab, out):   # the lanes' registers as A, B, C and the result D
    K, kind, ebits, na, nb, acc, epr, nc = layout(shape)
    per = na + nb + nc + (2 if shape in BS else 0); emask = (1 << ebits) - 1
    A = [[0] * K for i in range(16)]; B = [[0] * 8 for i in range(K)]; C = [[0] * 8 for i in range(16)]; D = [[0] * 8 for i in range(16)]
    for lane in range(32):
        g = lane >> 2; t = lane & 3; w = tab[per * lane:per * lane + per]
        for r in range(na):
            for i in range(epr): A[g + 8 * (r & 1)][epr * t + i + 4 * epr * (r >> 1)] = w[r] >> ebits * i & emask
        for r in range(nb):
            for i in range(epr): B[epr * t + i + 4 * epr * r][g] = w[na + r] >> ebits * i & emask
        for i in range(4):
            row = g + 8 * (i >> 1); col = 2 * t + (i & 1)
            if acc == 'f16':
                C[row][col] = w[na + nb + (i >> 1)] >> 16 * (i & 1) & 0xffff; D[row][col] = out[nc * lane + (i >> 1)] >> 16 * (i & 1) & 0xffff
            else:
                C[row][col] = w[na + nb + i]; D[row][col] = out[nc * lane + i]
    return A, B, C, D

def sets():
    r = []
    f = open(HERE + '/hw/mma.txt')
    while True:
        h = f.readline().split()
        if not h: break
        r.append((h[0], h[1], [int(x, 16) for x in f.readline().split()], [int(x, 16) for x in f.readline().split()]))
    return r

def kernel(src, shape, name):   # the kernel of g_mma.ptx for a shape, under another name
    base = 'bs1x' if shape in ALT else shape
    k = re.search(r'\.visible \.entry %s\(.*?\n}\n' % base, src, re.S).group(0)
    if shape in ALT: k = k.replace('.e4m3.e4m3.', '.%s.%s.' % tuple(x[:4] for x in ALT[shape]))
    return k.replace('.entry %s(' % base, '.entry %s(' % name).replace('tab_%s;' % base, 'tab_%s;' % name)

def ptx(shape, tab):
    K, kind, ebits, na, nb, acc, epr, nc = layout(shape)
    src = open(HERE + '/g_mma.ptx').read()
    k = kernel(src, shape, shape)
    hdr = re.search(r'\.version(?:.|\n)*?\.address_size 64\n', src).group(0)
    return '//! run %s 1 1 1 32 1 1 0 %d\n%s.global .align 4 .u32 tab_%s[%d] = {%s};\n%s' % (
        shape, 32 * nc, hdr, shape, len(tab), ', '.join('0x%x' % v for v in tab), k)

def rnd(kind, spread):
    (eb, mb) = FMT[kind]; bias = (1 << (eb - 1)) - 1
    if eb < 4: return random.getrandbits(4 if kind == 'e2m1' else 8)
    v = random.getrandbits(1) << (eb + mb) | random.randint(max(1, bias - spread), min((1 << eb) - 2, bias + spread)) << mb | random.getrandbits(mb)
    return v << 13 | random.getrandbits(13) if kind == 'tf32' else v

cmd = sys.argv[1] if len(sys.argv) > 1 else ''
if cmd == 'check':
    bad = 0; n = 0
    for (name, shape, tab, out) in sets():
        K, kind, ebits, na, nb, acc, epr, nc = layout(shape)
        A, B, C, D = matrices(shape, tab, out)
        b = sum(hwsum(kind, acc, products(shape, tab, A, B, i, j), C[i][j]) != D[i][j] for i in range(16) for j in range(8))
        if b: print('%s (%s): %d of 128 are not the model\'s' % (name, shape, b))
        bad += b; n += 128
    print('mmahw: %d words of the device, %d not as the model has them' % (n, bad))
elif cmd == 'ptx':
    for (name, shape, tab, out) in sets():
        if name == sys.argv[2]:
            open('%s/%s.ptx' % (sys.argv[3], name), 'w').write(ptx(shape, tab))
            f = open('%s/%s.exp' % (sys.argv[3], name), 'w'); f.write('%s: 0\n' % shape)
            for i in range(len(out)): f.write('%08x%s' % (out[i], '\n' if i % 8 == 7 or i == len(out) - 1 else ' '))
elif cmd == 'test':
    # every recorded set as a kernel of one module that asks ptxt for the hardware's sums
    src = open(HERE + '/g_mma.ptx').read()
    hdr = re.search(r'\.version(?:.|\n)*?\.address_size 64\n', src).group(0)
    runs = []; body = []; exp = []
    for (name, shape, tab, out) in sets():
        k = kernel(src, shape, name)
        runs.append('//! run %s 1 1 1 32 1 1 0 %d\n' % (name, len(out)))
        body.append('.global .align 4 .u32 tab_%s[%d] = {%s};\n%s' % (name, len(tab), ', '.join('0x%x' % v for v in tab), k))
        exp.append('%s: 0\n' % name + ''.join('%08x%s' % (out[i], '\n' if i % 8 == 7 or i == len(out) - 1 else ' ') for i in range(len(out))))
    open(sys.argv[2] + '/h_mma.ptx', 'w').write('// made by mmahw.py test: mma of data that does not add up exactly, and what an RTX PRO 4000 gave\n//! hw\n' + ''.join(runs) + hdr + ''.join(body))
    open(sys.argv[2] + '/h_mma.exp', 'w').write(''.join(exp))
elif cmd == 'new':
    shape = sys.argv[2]; random.seed(int(sys.argv[3])); spread = int(sys.argv[4]); cs = int(sys.argv[5])
    K, kind, ebits, na, nb, acc, epr, nc = layout(shape)
    tab = []
    for lane in range(32):
        for r in range(na + nb):
            kd = kind if type(kind) is str else kind[r >= na]
            tab.append(sum(rnd(kd, spread) << ebits * i for i in range(epr)))
        for r in range(nc): tab.append(0 if not cs else rnd(acc, cs) if acc == 'f32' else rnd(acc, cs) | rnd(acc, cs) << 16)
        if shape in BS:
            for r in range(2): tab.append(sum(random.randint(127 - spread, 127 + spread) << 8 * i for i in range(4)))
    open(sys.argv[6], 'w').write(ptx(shape, tab))
elif cmd == 'add':
    src = open(sys.argv[3]).read()
    m = re.search(r'tab_(\w+)\[\d+\] = \{([^}]*)\}', src)
    out = open(sys.argv[4]).read().split('\n')
    if not re.match(r'^\w+: 0$', out[0]): sys.exit('the device did not run it: ' + out[0])
    f = open(HERE + '/hw/mma.txt', 'a')
    f.write('%s %s\n%s\n%s\n' % (sys.argv[2], m.group(1), ' '.join('%x' % int(x, 16) for x in m.group(2).split(', ')), ' '.join(' '.join(out[1:]).split())))
else:
    sys.exit('usage: mmahw.py check | ptx NAME DIR | test DIR | new SHAPE SEED SPREAD CSPREAD OUT.ptx | add NAME FILE.ptx FILE.out')
