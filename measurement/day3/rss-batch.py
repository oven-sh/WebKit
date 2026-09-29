#!/usr/bin/env python3
# usage: rss-batch.py <small|full> <runs>
# Memory of the process when the tsc workload ends (MemoryFootprint() of the jsc shell), in turns, with a
# startup deferral scale of 10 and the compilers on the main thread.
import json, subprocess, sys, statistics
which, runs = sys.argv[1], int(sys.argv[2])
J = "/workspace/wkbuild/release-new/bin/jsc"
BASE = ["--useConcurrentJIT=0", "--useDollarVM=1", "--thresholdForJITAfterWarmUp=500", "--startupJITDeferralScale=10"]
OFF = ["--missCountForLLIntTierUp=0", "--useLLIntStringLengthFastPath=0"]
configs = [("four off", OFF), ("defaults", []), ("defaults, deferral kept", ["--useStartupJITDeferralAfterLLIntMisses=1"]), ("no scale, four off", ["--startupJITDeferralScale=1"] + OFF)]
res = {n: [] for n, _ in configs}
for r in range(runs):
    for name, flags in configs:
        out = subprocess.run([J] + BASE + flags + ["rss-end.js", "--", "1", which], cwd="/workspace/wkbuild/workloads", capture_output=True, text=True).stdout
        line = [l for l in out.splitlines() if l.startswith("MEM ")][-1]
        res[name].append(json.loads(line[4:]))
for name, _ in configs:
    cur = [x["current"] / 1048576 for x in res[name]]
    peak = [x["peak"] / 1048576 for x in res[name]]
    jit = [x["jitBytes"] / 1048576 for x in res[name]]
    print("%-26s current %.1f MiB [%.1f..%.1f]  peak %.1f [%.1f..%.1f]  jit code %.2f  baseline %d" % (name, statistics.median(cur), min(cur), max(cur), statistics.median(peak), min(peak), max(peak), statistics.median(jit), res[name][0]["baseline"]), flush=True)
