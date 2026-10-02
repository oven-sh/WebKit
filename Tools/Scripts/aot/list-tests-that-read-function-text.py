#!/usr/bin/env python3
"""Makes JSTests/bun-tests-that-read-function-text.txt: the tests of JSTests/stress that pass in the interpreter but fail there once
Function.prototype.toString() no longer gives the text of the program's functions.

    list-tests-that-read-function-text.py <jsc> [--jobs N] > JSTests/bun-tests-that-read-function-text.txt
"""
import argparse
import concurrent.futures
import os
import re
import subprocess

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
STRESS = os.path.join(ROOT, "JSTests", "stress")
HEADER = """# Tests that are skipped in the aot and aot-validate modes of run-jsc-stress-tests.
#
# A program that is compiled ahead of time has no source text, so Function.prototype.toString() says of its functions what it says of
# native ones. Each test here depends on the text.
#
# How the list was made: Tools/Scripts/aot/list-tests-that-read-function-text.py. These pass with --useImmutableIntrinsics=1 --useJIT=0 and
# fail with --hideFunctionSourceForTesting=1 as well, in the plain interpreter (no AOT).
"""


def passes(jsc, options, test):
    try:
        return not subprocess.run([jsc, *options, test], cwd=STRESS, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60).returncode
    except subprocess.TimeoutExpired:
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("jsc")
    parser.add_argument("--jobs", type=int, default=3)
    arguments = parser.parse_args()
    jsc = os.path.abspath(arguments.jsc)

    def reads_text(test):
        with open(os.path.join(STRESS, test), errors="replace") as file:
            text = file.read()
        if not re.search(r"toString|String\(|`|\+ *\"|\" *\+|' *\+|\+ *'", text):
            return False
        options = re.findall(r'"(--[^"]*)"', " ".join(line for line in text.split("\n")[:6] if line.startswith("//@ ")))
        base = ["--useImmutableIntrinsics=1", "--useJIT=0", *options]
        hidden = [*base, "--hideFunctionSourceForTesting=1"]
        # (Twice: on a busy machine a slow test may time out once.)
        return passes(jsc, base, test) and not passes(jsc, hidden, test) and not passes(jsc, hidden, test)

    tests = sorted(name for name in os.listdir(STRESS) if name.endswith(".js"))
    with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
        found = [test for test, reads in zip(tests, pool.map(reads_text, tests)) if reads]
    print(HEADER)
    for test in found:
        print(f"stress/{test}")


if __name__ == "__main__":
    main()
