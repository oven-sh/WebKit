#!/usr/bin/env python3
"""Tests the tests: leaves out one guard of the compiler at a time and sees whether anything notices.

  drop-guards.py <jsc> [--only REGEX] [--fuzz SECONDS] [--jobs N] [--seed N]

Nearly every check that compiled code makes is a branch to a block that is rarely taken. --aotGuardToDropForTesting=<file>:<line>
leaves out the one that is emitted at that place in the source of the compiler, in every function, so that the likely block is
always taken. That compiler is wrong, and no rebuilding is needed to have it.

For every such place in Source/JavaScriptCore/aot/AOTLower*: run-tests.py; if that passes, how many functions of the tests have the
guard at all; and then fuzz-programs.py for a while. One line for each:

  tests      a test fails
  fuzzing    the tests pass, a generated program differs (its generator and seed are in the name of the finding)
  SURVIVES   nothing notices, though the guard is in the given number of functions of the tests: a test is missing
  unreached  nothing notices, and neither a test nor any of forty generated programs has the guard: a test is missing that gets
             the compiler there at all (the guards of typed layouts need a type table, which only an embedder's tests have)

A guard may survive because it cannot fail, being implied by another. Then it can go.
"""
import argparse, os, re, subprocess, sys, tempfile, shutil
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.realpath(os.path.join(HERE, '..', '..', '..'))
AOT = os.path.join(ROOT, 'Source', 'JavaScriptCore', 'aot')
STRESS = os.path.join(ROOT, 'JSTests', 'stress')

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('jsc', type=os.path.abspath)
parser.add_argument('--only', default='')
parser.add_argument('--fuzz', type=float, default=40)
parser.add_argument('--jobs', type=int, default=os.cpu_count() // 2)
parser.add_argument('--seed', type=int, default=1)
args = parser.parse_args()

sites = []
for name in sorted(os.listdir(AOT)):
    if not re.match(r'AOTLower.*\.(cpp|h)$', name):
        continue
    for number, line in enumerate(open(os.path.join(AOT, name)), 1):
        if 'rarely(' in line and 'branch(' in line:
            sites.append(('%s:%d' % (name, number), line.strip()))
sites = [s for s in sites if re.search(args.only, s[0])]
tests = [f for f in sorted(os.listdir(STRESS)) if re.match(r'(aot|sound-types)-.*\.js$', f)]


def functionsWithGuard(site, programs):
    def one(path):
        module = ['-m'] if re.search(r'//@[^\n]*"-m"', open(path, errors='replace').read(2000)) else []
        p = subprocess.run([args.jsc, '--useJIT=false', '--useDollarVM=true', '--writeAOTImageTo=/dev/null', '--verboseAOTCompilation=1', '--aotGuardToDropForTesting=' + site, *module, path], cwd=STRESS, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return p.stdout.count(b'AOT: dropped the guard')
    with ThreadPoolExecutor(args.jobs) as pool:
        return sum(pool.map(one, programs))


source = open(os.path.join(HERE, 'fuzz-programs.py')).read()
generators = {'__file__': os.path.join(HERE, 'fuzz-programs.py')}
exec(compile(source[:source.index('parser = argparse.ArgumentParser')], 'fuzz-programs.py', 'exec'), generators)
samples = tempfile.mkdtemp()
generated = []
for name in ('Closures', 'Objects', 'Loops', 'Numbers', 'Classes'):
    for seed in range(8):
        generated.append(os.path.join(samples, '%s-%d.js' % (name, seed)))
        open(generated[-1], 'w').write(generators[name](args.seed * 100 + seed).program())


tally = {}
for site, line in sites:
    env = dict(os.environ, JSC_aotGuardToDropForTesting=site)
    p = subprocess.run([sys.executable, os.path.join(HERE, 'run-tests.py'), args.jsc], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=tempfile.gettempdir())
    m = re.search(rb'(\d+) pass, (\d+) fail', p.stdout)
    if not m or int(m.group(2)):
        verdict = 'tests      %s fail' % (m.group(2).decode() if m else '?')
    else:
        count = functionsWithGuard(site, [os.path.join(STRESS, test) for test in tests])
        found = []
        if count or functionsWithGuard(site, generated):
            out = tempfile.mkdtemp()
            subprocess.run([sys.executable, os.path.join(HERE, 'fuzz-programs.py'), args.jsc, out, '--seconds', str(args.fuzz), '--jobs', str(args.jobs), '--seed', str(args.seed)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            found = sorted(f for f in os.listdir(out) if not f.startswith('case-'))
            shutil.rmtree(out)
        verdict = 'fuzzing    %d findings, %s' % (len(found), found[0]) if found else 'SURVIVES   in %d functions' % count if count else 'unreached'
    tally[verdict.split()[0]] = tally.get(verdict.split()[0], 0) + 1
    print('%-28s %-34s %s' % (site, verdict, line[:110]), flush=True)
shutil.rmtree(samples)
print(tally)
