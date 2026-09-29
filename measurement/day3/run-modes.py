#!/usr/bin/env python3
# usage: run-modes.py <jsc> <test.js>... [-- extra flags]
# Runs each test once for each "//@ runDefault(...)" line of it, with the flags of that line and the base flags
# of run-jsc-stress-tests. A test with no such line runs once with the base flags.
import re, subprocess, sys, os, time

BASE = ["--useFTLJIT=false", "--useFunctionDotArguments=true", "--validateExceptionChecks=true",
        "--useDollarVM=true", "--maxPerThreadStackUsage=1572864"]

def modes_of(path):
    modes = []
    with open(path) as f:
        for line in f:
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

def main():
    args = sys.argv[1:]
    extra = []
    if "--" in args:
        i = args.index("--")
        extra = args[i + 1:]
        args = args[:i]
    jsc, tests = args[0], args[1:]
    failed = 0
    total = 0
    for test in tests:
        for mode in modes_of(test):
            total += 1
            cmd = [jsc] + BASE + mode + extra + [os.path.basename(test)]
            start = time.time()
            try:
                p = subprocess.run(cmd, cwd=os.path.dirname(os.path.abspath(test)), capture_output=True, text=True, timeout=1200)
                code, out = p.returncode, (p.stdout + p.stderr)
            except subprocess.TimeoutExpired:
                code, out = -999, "timeout"
            took = time.time() - start
            status = "ok  " if code == 0 else "FAIL"
            if code != 0:
                failed += 1
            print("%s %6.1fs %s %s" % (status, took, os.path.basename(test), " ".join(mode + extra)), flush=True)
            if code != 0:
                print("     exit %d: %s" % (code, out.strip()[-1500:]), flush=True)
    print("%d runs, %d failed" % (total, failed))
    sys.exit(1 if failed else 0)

main()
