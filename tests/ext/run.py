#!/usr/bin/env python3
"""Runs external conformance suites against mcc.

usage: run.py <suite> [--mcc path] [--only substr] [-v] [--list-fail file]
suites: ctestsuite, tcc, gcc, llvm
A test passes when it compiles, runs (exit status 0 unless the suite says
otherwise) and, where the suite has expected output, matches it.
"""
import os, subprocess, sys, tempfile, re, concurrent.futures

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TP = os.path.join(ROOT, 'third_party')

def read(p):
    with open(p, 'rb') as f:
        return f.read()

def suite_ctestsuite():
    d = os.path.join(TP, 'c-testsuite/tests/single-exec')
    for n in sorted(os.listdir(d)):
        if n.endswith('.c'):
            p = os.path.join(d, n)
            yield dict(name=n, src=p, expect=read(p + '.expected'), cwd=d)

def suite_tcc():
    d = os.path.join(TP, 'tinycc/tests/tests2')
    for n in sorted(os.listdir(d)):
        if n.endswith('.c') and os.path.exists(os.path.join(d, n[:-2] + '.expect')):
            p = os.path.join(d, n)
            yield dict(name=n, src=p, expect=read(p[:-2] + '.expect'), cwd=d, anyexit=True, merge=True)

def suite_gcc():
    d = os.path.join(TP, 'gcc/gcc/testsuite/gcc.c-torture/execute')
    for sub in ('', 'ieee'):
        dd = os.path.join(d, sub)
        for n in sorted(os.listdir(dd)):
            if n.endswith('.c'):
                yield dict(name=os.path.join(sub, n), src=os.path.join(dd, n), expect=None, cwd=dd)

def suite_gcc_compile():
    d = os.path.join(TP, 'gcc/gcc/testsuite/gcc.c-torture/compile')
    for n in sorted(os.listdir(d)):
        if n.endswith('.c'):
            yield dict(name=n, src=os.path.join(d, n), expect=None, cwd=d, compile_only=True)

def suite_llvm():
    base = os.path.join(TP, 'llvm-test-suite/SingleSource')
    for sub in ('UnitTests', 'Regression/C'):
        for dp, dn, fn in os.walk(os.path.join(base, sub)):
            for n in sorted(fn):
                if n.endswith('.c'):
                    p = os.path.join(dp, n)
                    ref = p[:-2] + '.reference_output'
                    yield dict(name=os.path.relpath(p, base), src=p,
                               expect=read(ref) if os.path.exists(ref) else None, cwd=dp, llvm=True)

SUITES = dict(ctestsuite=suite_ctestsuite, tcc=suite_tcc, gcc=suite_gcc, llvm=suite_llvm)
SUITES['gcc-compile'] = suite_gcc_compile

def run_one(t, mcc, tmp):
    exe = os.path.join(tmp, re.sub(r'[^A-Za-z0-9_.]', '_', t['name']) + '.exe')
    try:
        if t.get('compile_only'):
            r = subprocess.run([mcc, '-c', '-o', exe + '.o', t['src']], capture_output=True, timeout=20, cwd=t['cwd'])
        else:
            r = subprocess.run([mcc, '-o', exe, t['src']], capture_output=True, timeout=20, cwd=t['cwd'])
    except subprocess.TimeoutExpired:
        return 'CTIMEOUT', ''
    if r.returncode != 0:
        kind = 'CRASH' if r.returncode < 0 or b'internal' in r.stderr else 'COMPILE'
        msg = r.stderr.decode('latin-1').strip().split('\n')
        return kind, msg[0] if msg else ''
    if t.get('compile_only'):
        return 'PASS', ''
    try:
        r = subprocess.run([exe], capture_output=True, timeout=10, cwd=t['cwd'], stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return 'TIMEOUT', ''
    finally:
        try: os.unlink(exe)
        except OSError: pass
    out = r.stdout
    if t.get('llvm'):
        out = out + b'exit %d\n' % r.returncode
    elif not t.get('anyexit') and r.returncode != 0:
        return 'RUNFAIL', 'exit %d' % r.returncode
    if t.get('merge'):
        out = r.stdout + r.stderr
    if t['expect'] is not None and out != t['expect']:
        if r.returncode < 0:
            return 'RUNFAIL', 'signal %d' % -r.returncode
        return 'OUTPUT', ''
    return 'PASS', ''

def main():
    args = sys.argv[1:]
    mcc = os.path.join(ROOT, 'build/stage0/mcc'); only = None; verbose = False; lf = None
    names = []
    while args:
        a = args.pop(0)
        if a == '--mcc': mcc = os.path.abspath(args.pop(0))
        elif a == '--only': only = args.pop(0)
        elif a == '-v': verbose = True
        elif a == '--list-fail': lf = args.pop(0)
        else: names.append(a)
    skip = set()
    sk = os.path.join(ROOT, 'tests/ext/skip.txt')
    reasons = {}
    if os.path.exists(sk):
        for line in open(sk):
            line = line.split('#')[0].split()
            if len(line) >= 1:
                skip.add(line[0])
    total_fail = 0
    for sname in names:
        tests = [t for t in SUITES[sname]() if not only or only in t['name']]
        skipped = [t for t in tests if sname + ':' + t['name'] in skip]
        tests = [t for t in tests if sname + ':' + t['name'] not in skip]
        counts = {}; fails = []
        with tempfile.TemporaryDirectory() as tmp:
            with concurrent.futures.ThreadPoolExecutor(8) as ex:
                for t, (st, msg) in zip(tests, ex.map(lambda t: run_one(t, mcc, tmp), tests)):
                    counts[st] = counts.get(st, 0) + 1
                    if st != 'PASS':
                        fails.append((t['name'], st, msg))
        for n, st, msg in fails:
            if verbose: print('%-8s %s  %s' % (st, n, msg[:150]))
        if lf:
            with open(lf, 'w') as f:
                for n, st, msg in fails: f.write('%s %s %s\n' % (st, n, msg[:200]))
        npass = counts.get('PASS', 0)
        print('%s: %d/%d passed, %d skipped  %s' % (sname, npass, len(tests), len(skipped),
              ' '.join('%s=%d' % kv for kv in sorted(counts.items()) if kv[0] != 'PASS')))
        total_fail += len(tests) - npass
    sys.exit(1 if total_fail else 0)

main()
