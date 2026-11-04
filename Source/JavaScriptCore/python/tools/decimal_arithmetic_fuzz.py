#!/usr/bin/env python3
"""Random operations for decimal_arithmetic.cpp, each compared with what the _decimal of the CPython that runs this script gives: result and flags.

usage: decimal_arithmetic_fuzz.py BINARY [seed] [count] [--ops a,b,c] [--maxprec N] [--big] [--show N]

--big: every precision is between 100 and --maxprec (long products, quotients and roots)."""
import sys, random, subprocess, argparse
import _decimal as D
ap = argparse.ArgumentParser()
ap.add_argument('binary'); ap.add_argument('seed', type=int, nargs='?', default=1); ap.add_argument('count', type=int, nargs='?', default=20000)
ap.add_argument('--ops'); ap.add_argument('--maxprec', type=int, default=60); ap.add_argument('--show', type=int, default=15)
ap.add_argument('--big', action='store_true')
A = ap.parse_args()
R = random.Random(A.seed)
ROUND = ['ROUND_UP','ROUND_DOWN','ROUND_CEILING','ROUND_FLOOR','ROUND_HALF_UP','ROUND_HALF_DOWN','ROUND_HALF_EVEN','ROUND_05UP']
UN = ['abs','plus','minus','sqrt','exp','ln','log10','next_minus','next_plus','normalize','to_integral_value','to_integral_exact','logical_invert','logb','copy_abs','copy_negate','number_class','to_eng_string']
BIN = ['add','subtract','multiply','divide','divide_int','remainder','remainder_near','max','max_mag','min','min_mag','next_toward','quantize','logical_and','logical_or','logical_xor','scaleb','shift','rotate','compare','compare_signal','power','compare_total','compare_total_mag','copy_sign','divmod','same_quantum']
TER = ['fma','powmod']
OTHER = ['create','format']
ALL = UN + BIN + TER + OTHER
ops = A.ops.split(',') if A.ops else ALL
BITS = {D.Clamped:1, D.DivisionByZero:4, D.Inexact:0x40, D.Overflow:0x800, D.Rounded:0x1000, D.Subnormal:0x2000, D.Underflow:0x4000, D.FloatOperation:0x400}
INVALID = 0x2|0x8|0x10|0x80|0x100|0x200

def digits(n, binary=False):
    if n <= 0: return '0'
    k = R.random()
    if binary: return ''.join(R.choice('01') for _ in range(n))
    if k < 0.1: return '9'*n
    if k < 0.2: return R.choice('123456789') + '0'*(n-1)
    if k < 0.3: return ''.join(R.choice('09') for _ in range(n))
    if k < 0.4: return ''.join(R.choice('05') for _ in range(n))
    return ''.join(R.choice('0123456789') for _ in range(n))

def number(ctx, kind=None):
    k = R.random()
    sign = R.choice(['', '', '-'])
    if k < 0.03: return sign + R.choice(['Infinity', 'NaN', 'sNaN', 'NaN123', 'sNaN9', 'NaN' + digits(R.randint(1, ctx.prec + 3))])
    if k < 0.08: return sign + '0' + R.choice(['', 'E%d' % R.randint(-50, 50), 'E%d' % R.randint(ctx.Emin*2-10, ctx.Emax*2+10), '.000'])
    nd = R.choice([1, 2, 3, R.randint(1, ctx.prec), R.randint(1, ctx.prec), ctx.prec, ctx.prec + 1, R.randint(1, 2*ctx.prec + 5), 19, 20, 38, 39])
    c = digits(nd)
    j = R.random()
    if j < 0.35: e = R.randint(-nd - 3, 5)
    elif j < 0.7: e = R.randint(-40, 40)
    elif j < 0.85: e = R.randint(ctx.Emin - ctx.prec - 5, ctx.Emax + 5)
    elif j < 0.95: e = R.choice([ctx.Emin, ctx.Emax, ctx.Etiny(), ctx.Etop(), ctx.Emin - nd, ctx.Emax - nd + 1, ctx.Emax - nd, ctx.Etiny() - 1]) + R.randint(-2, 2)
    else: e = R.randint(-10**R.randint(1, 17), 10**R.randint(1, 17))
    return '%s%sE%d' % (sign, c, e)

def small_int(ctx):
    k = R.random()
    if k < 0.6: return str(R.randint(-ctx.prec - 2, ctx.prec + 2))
    if k < 0.8: return str(R.randint(-1000, 1000))
    return number(ctx)

def context():
    k = R.random()
    prec = R.choice([1, 2, 3, 9, 16, 19, 20, 28, 28, 34, 38, 39, R.randint(1, A.maxprec), R.randint(1, A.maxprec)])
    if A.big: prec = R.randint(100, A.maxprec)
    if k < 0.3: emax, emin = 999999, -999999
    elif k < 0.6:
        emax = R.randint(0, 100); emin = -R.randint(0, 100)
    elif k < 0.8: emax = R.randint(0, 10); emin = -R.randint(0, 10)
    elif k < 0.9: emax, emin = D.MAX_EMAX, D.MIN_EMIN
    else: emax = R.randint(0, 10**R.randint(1, 17)); emin = -R.randint(0, 10**R.randint(1, 17))
    c = D.Context(prec=prec, rounding=R.choice(ROUND), Emin=emin, Emax=emax, clamp=R.choice([0, 0, 1]), traps=[], flags=[])
    if c.clamp and c.Etop() < 0: c.clamp = 0
    return c

FMT_TYPES = ['', 'e', 'E', 'f', 'F', 'g', 'G', '%']
def fmt():
    s = ''
    if R.random() < 0.4: s += R.choice(['', '*', 'x', '0']) + R.choice('<>=^')
    s += R.choice(['', '', '+', '-'])
    if R.random() < 0.2 and not any(ch in s for ch in '<>=^'): s += '0'
    if R.random() < 0.5: s += str(R.randint(1, 40))
    if R.random() < 0.3: s += ','
    if R.random() < 0.6: s += '.' + str(R.randint(0, 30))
    s += R.choice(FMT_TYPES)
    return s

cases = []
for i in range(A.count):
    op = R.choice(ops)
    ctx = context()
    if op in UN:
        args = [number(ctx)]
        if op == 'logical_invert' and R.random() < 0.8: args = [digits(R.randint(1, ctx.prec + 3), True)]
        if op in ('exp',) and R.random() < 0.7: args = ['%s%sE%d' % (R.choice(['', '-']), digits(R.randint(1, ctx.prec + 2)), R.randint(-ctx.prec - 5, 3) - R.randint(0, ctx.prec))]
        if op in ('ln', 'log10', 'sqrt') and R.random() < 0.8: args[0] = args[0].lstrip('-')
        if op in ('ln', 'log10') and R.random() < 0.15: args = [R.choice(['1.' + '0'*R.randint(0, ctx.prec+3) + digits(R.randint(1, 5)), '0.' + '9'*R.randint(1, ctx.prec+3) + digits(R.randint(0, 4)), '1E%d' % R.randint(-50, 50), '10', '1'])]
    elif op in BIN:
        args = [number(ctx), number(ctx)]
        if op.startswith('logical') and R.random() < 0.8: args = [digits(R.randint(1, ctx.prec + 3), True), digits(R.randint(1, ctx.prec + 3), True)]
        if op in ('scaleb', 'shift', 'rotate'): args[1] = small_int(ctx)
        if op == 'power':
            k = R.random()
            if k < 0.4: args[1] = str(R.randint(-200, 200))
            elif k < 0.7: args = ['%sE%d' % (digits(R.randint(1, ctx.prec)), R.randint(-ctx.prec - 2, 3)), '%s%sE%d' % (R.choice(['', '-']), digits(R.randint(1, 6)), R.randint(-6, 2))]
            elif k < 0.8: args[0] = R.choice(['1', '1.000', '-1', '1.0', '10', '0.1', '2', '0.5', '100', '-1.00'])
        if op in ('remainder', 'remainder_near', 'divide_int', 'divmod') and R.random() < 0.7:
            e = R.randint(-10, 10)
            args = ['%s%sE%d' % (R.choice(['', '-']), digits(R.randint(1, ctx.prec + 2)), e + R.randint(-3, 3)), '%s%sE%d' % (R.choice(['', '-']), digits(R.randint(1, ctx.prec + 2)), e)]
        if op == 'quantize' and R.random() < 0.7: args[1] = '1E%d' % R.randint(-ctx.prec - 3, ctx.prec + 3)
        if R.random() < 0.03: args[1] = args[0]
    elif op == 'fma': args = [number(ctx), number(ctx), number(ctx)]
    elif op == 'powmod':
        args = [str(R.randint(-10**R.randint(1, ctx.prec + 2), 10**R.randint(1, ctx.prec + 2))), str(R.randint(-2, 10**R.randint(1, 12))), str(R.randint(-10**R.randint(1, ctx.prec + 1), 10**R.randint(1, ctx.prec + 1)))]
        if R.random() < 0.2:
            x = number(ctx)
            if 'E' not in x or abs(int(x.split('E')[1])) < 3000: args[R.choice([0, 2])] = x
        if R.random() < 0.05: args[1] = R.choice(['NaN', 'sNaN', 'Infinity', '-Infinity', '0', '-0', '1.5', '2.0', '0E5', '1E2'])
        if R.random() < 0.2: args[0] += 'E%d' % R.randint(0, 30)
        if R.random() < 0.1: args[1] += 'E%d' % R.randint(0, 3)
    elif op == 'create':
        s = number(ctx)
        k = R.random()
        if k < 0.3:
            nd = R.randint(1, 2*ctx.prec+3); c = digits(nd); p = R.randint(0, nd)
            s = R.choice(['', '-', '+']) + c[:p] + '.' + c[p:] + R.choice(['', 'e%d' % R.randint(-30, 30), 'E+%d' % R.randint(0, 30), 'e', 'E-', 'e99999999999999999999999', 'e-99999999999999999999999'])
        elif k < 0.4: s = R.choice(['', '.', '-', '+', 'e5', '.e5', '1e', '1.e5', '.5', '5.', 'inf', 'INFINITY', 'infinit', 'nAn', 'SnaN00012', 'nan1x', '--1', '1..2', '1e5.0', '0x10', 'NaN0', '00', '0.0', '00.00e-5', '1_000', 'infinity1', 'sNaN', '-sNaN000'])
        if not s or ' ' in s: s = 'x'
        args = [s]
    elif op == 'format':
        args = [number(ctx), fmt()]
        if 'E' in args[0] and abs(int(args[0].split('E')[1])) > 5000: args[0] = args[0].split('E')[0]
        if R.random() < 0.6: args[0] = '%s%sE%d' % (R.choice(['', '-']), digits(R.randint(1, 30)), R.randint(-35, 12))
    cases.append((op, ctx, args))

inp = '\n'.join('%s %d %d %d %d %d %s' % (op, c.prec, ROUND.index(c.rounding), c.Emin, c.Emax, c.clamp, ' '.join(a)) for op, c, a in cases) + '\n'
p = subprocess.run([A.binary], input=inp, capture_output=True, text=True)
if p.returncode:
    print('BINARY FAILED', p.returncode, p.stderr[-3000:])
    done = p.stdout.count('\n')
    print('failed at case', done, inp.split('\n')[done])
    sys.exit(2)
outs = p.stdout.split('\n')
bad = 0
maxc = D.Context(prec=D.MAX_PREC, Emax=D.MAX_EMAX, Emin=D.MIN_EMIN, traps=[])
for (op, ctx, args), got in zip(cases, outs):
    try:
        if op == 'create':
            r = str(ctx.create_decimal(args[0]))
        else:
            xs = [D.Decimal(a, maxc) for a in (args[:1] if op == 'format' else args)]
            if op == 'powmod': r = str(ctx.power(xs[0], xs[1], xs[2]))
            elif op == 'divmod': r = ','.join(map(str, ctx.divmod(*xs)))
            elif op == 'format':
                D.setcontext(ctx)
                try: r = format(xs[0], args[1])
                except ValueError: r = '<fail>'
                ctx.clear_flags()
            else: r = str(getattr(ctx, op)(*xs))
    except MemoryError:
        r = '<memory>'
    want = 0
    for e, b in BITS.items():
        if ctx.flags[e]: want |= b
    inv = ctx.flags[D.InvalidOperation]
    text, _, st = got.rpartition('|')
    st = int(st, 16) if st else -1
    gotinv = bool(st & INVALID)
    if op == 'format':
        if text == '<badspec>': continue   # falls back to _pydecimal in the module
        ok = text == r
    else:
        ok = text == r and (st & ~INVALID) == want and gotinv == inv
    if r == '<memory>': ok = bool(st & 0x200) or op == 'powmod'
    if not ok:
        bad += 1
        if bad <= A.show:
            print('DIFF %s prec=%d %s Emin=%d Emax=%d clamp=%d %s\n   want %s %x inv=%s\n   got  %s %x' % (op, ctx.prec, ctx.rounding, ctx.Emin, ctx.Emax, ctx.clamp, args, r, want, inv, text, st))
print('seed', A.seed, 'cases', len(cases), 'differences', bad)
sys.exit(1 if bad else 0)
