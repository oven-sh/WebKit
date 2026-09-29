#!/usr/bin/env python3
# usage: time-modes.py <jsc> <test.js>...
# CPU time (user + system, all threads) of each //@ runDefault mode of each test. The machine is loaded, so the
# wall clock says little. JSTests/README.md: a test runs in under 200 ms in all configurations.
import re, subprocess, sys, os, resource

BASE = ["--useFTLJIT=false", "--useFunctionDotArguments=true", "--validateExceptionChecks=true",
        "--useDollarVM=true", "--maxPerThreadStackUsage=1572864"]

def modes_of(path):
    modes = []
    for line in open(path):
        if not line.startswith("//@"):
            if line.strip() and not line.startswith("//"):
                break
            continue
        m = re.match(r'//@\s*runDefault\((.*)\)\s*$', line)
        if m:
            modes.append(re.findall(r'"([^"]*)"', m.group(1)))
        elif re.match(r'//@\s*runDefault\s*$', line):
            modes.append([])
    return modes or [[]]

def cpu_of(cmd, cwd):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=1200)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    return p.returncode, (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)

jsc, tests = sys.argv[1], sys.argv[2:]
empty = '/tmp/empty-for-time-modes.js'
open(empty, 'w').write('')
floor = min(cpu_of([jsc] + BASE + [empty], '/tmp')[1] for _ in range(5))
print("empty script: %.0f ms" % (floor * 1000))
worst = 0
for test in tests:
    for mode in modes_of(test):
        runs = [cpu_of([jsc] + BASE + mode + [os.path.basename(test)], os.path.dirname(os.path.abspath(test))) for _ in range(3)]
        code = max(r[0] for r in runs)
        cpu = min(r[1] for r in runs)
        worst = max(worst, cpu)
        print("%s %5.0f ms  %s %s" % ("ok  " if code == 0 else "FAIL", cpu * 1000, os.path.basename(test), " ".join(mode)), flush=True)
print("worst: %.0f ms" % (worst * 1000))
