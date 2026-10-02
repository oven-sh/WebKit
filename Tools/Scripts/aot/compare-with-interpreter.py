#!/usr/bin/env python3
"""Runs WebKit's JSTests/stress interpreted and compiled ahead of time, and reports the tests that behave differently.

    compare-with-interpreter.py <jsc> [--first N] [--count N] [--jobs N]

The whole directory takes a few minutes with three jobs. It needs nothing but a jsc, so it is the quickest way to try a new
platform: CI's artifact `jsc-shell-<platform>` in a container will do.

The reference is the interpreter with immutable intrinsics and without the text of functions, both of which ahead-of-time
compilation implies. A test counts only if it passes that way. It is `same` if, compiled ahead of time, it exits with 0 and prints the same.

Left out: tests whose header skips them or asks for a particular way of running, and tests that look at the engine itself (the
tiers, the collector, other scripts). Of what differs, expect the tests that run-jsc-stress-tests skips in its modes `aot` and
`aot-validate`: those that read f.caller or f.arguments ($skipModes in their headers) and those in
JSTests/bun-tests-that-change-builtins.txt. They are marked.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
STRESS = os.path.join(ROOT, "JSTests", "stress")

RUNS_ITS_OWN_WAY = re.compile(
    r"^//@ *(skip|runFTL|runNoFTL|runBytecodeCache|slow|requireOptions|runDefault\(|runWebAssembly|runMisc|runNoisy|defaultNoEagerRun|runLayout|crashOK|exclusive)",
    re.M,
)
LOOKS_AT_THE_ENGINE = re.compile(
    r"\$vm|load\(|readFile|checkModuleSyntax|runString|createGlobalObject|Loader|drainMicrotasks|setTimeout|waitFor|agent"
    r"|edenGC|fullGC|gc\(\)|noFTL|noDFG|numberOfDFGCompiles|jscOptions|getOptions|setOption"
)


def run(jsc, options, test, seconds):
    try:
        result = subprocess.run([jsc, *options, test], cwd=STRESS, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=seconds)
        return result.returncode, result.stdout
    except subprocess.TimeoutExpired:
        return "timeout", b""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("jsc")
    parser.add_argument("--first", type=int, default=1)
    parser.add_argument("--count", type=int, default=1 << 30)
    parser.add_argument("--jobs", type=int, default=3)
    arguments = parser.parse_args()
    jsc = os.path.abspath(arguments.jsc)

    with open(os.path.join(ROOT, "JSTests", "bun-tests-that-change-builtins.txt")) as file:
        changes_builtins = {line.strip().split("/")[-1] for line in file if line.startswith("stress/")}

    def compare(test):
        with open(os.path.join(STRESS, test), errors="replace") as file:
            text = file.read()
        head = "\n".join(text.split("\n")[:3])
        if RUNS_ITS_OWN_WAY.search(head) or LOOKS_AT_THE_ENGINE.search(text):
            return "left out", test, ""
        code, expected = run(jsc, ["--useImmutableIntrinsics=1", "--hideTextOfFunctionsForTesting=1", "--useJIT=0"], test, 40)
        if code != 0:
            return "fails interpreted", test, ""
        code, actual = run(jsc, ["--compileMainScriptAheadOfTime=1", "--useJIT=0"], test, 80)
        if code == 0 and actual == expected:
            return "same", test, ""
        known = " (skipped in the aot modes)" if test in changes_builtins or re.search(r"skipModes.*:aot", head) else ""
        first_line = actual.decode(errors="replace").split("\n")[0][:110]
        return "DIFFERS", test, f"exit {code} {first_line}{known}"

    tests = sorted(f for f in os.listdir(STRESS) if f.endswith(".js"))[arguments.first - 1 :][: arguments.count]
    counts = collections.Counter()
    unexpected = 0
    with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
        for verdict, test, detail in pool.map(compare, tests):
            counts[verdict] += 1
            if verdict == "DIFFERS":
                print(f"DIFFERS {test} {detail}")
                unexpected += not detail.endswith("(skipped in the aot modes)")
    print("; ".join(f"{count} {verdict}" for verdict, count in sorted(counts.items())))
    print(f"{unexpected} differ that the aot modes do not skip")
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
