#!/usr/bin/env python3
"""Runs the engine's own tests of ahead-of-time compilation and sound types. About 10 seconds.

    run-tests.py <jsc>

The tests are JSTests/stress/aot-*.js and sound-types-*.js. Each runs with the options of its header, three times: as it says; with
every inferred type checked against the value and the B3 and Air validators on, as in the mode `aot-validate`; and without data stubs,
which is what a CPU other than ARM64 and x86-64 would compile. A `//@ run("name", ...)` in the header is one more run.

A test fails if it prints anything or exits with anything but 0. run-javascriptcore-tests runs all of this and much more, in
the modes `aot` and `aot-validate` among the rest. This is what is quick enough to run after every change.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STRESS = os.path.realpath(os.path.join(HERE, "..", "..", "..", "JSTests", "stress"))

def header_of(path):
    with open(path, errors="replace") as file:
        return [line for line in file.read().split("\n")[:6] if line.startswith("//@ ")]


def run(jsc, options, test):
    """Returns None if the test passed, or a line that says how it did not."""
    if "-m" in options:
        options = [option for option in options if option != "-m"] + ["-m"]
    command = [sys.executable, os.path.join(HERE, "capped.py"), "4", "120", jsc, *options, test]
    result = subprocess.run(command, cwd=STRESS, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    output = [line for line in result.stdout.split("\n") if line and not line.startswith("[capped]")]
    if not result.returncode and not output:
        return None
    return f"exit {result.returncode} {output[0][:120] if output else ''}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("jsc")
    arguments = parser.parse_args()
    jsc = os.path.abspath(arguments.jsc)

    tests = sorted(os.path.basename(path) for pattern in ("aot-*.js", "sound-types-*.js") for path in glob.glob(os.path.join(STRESS, pattern)))
    runs = []
    for test in tests:
        header = header_of(os.path.join(STRESS, test))
        options = re.findall(r'"(--?[^"]*)"', " ".join(line for line in header if not line.startswith("//@ run(")))
        for line in header:
            if also := re.match(r'//@ run\("([^"]+)"(.*)\)', line):
                runs.append((test, options + re.findall(r'"(--?[^"]*)"', also.group(2)), also.group(1)))
        runs.append((test, options, "as it says"))
        if not any("$skipModes << :aot_validate" in line for line in header):
            runs.append((test, options + ["--validateAOTInferredTypes=true", "--validateGraphAtEachPhase=true", "--aotTypeCoveragePath="], "validated"))
        runs.append((test, options + ["--useAOTDataStubs=false"], "without data stubs"))

    failures = 0
    for test, options, label in runs:
        problem = run(jsc, options, test)
        if problem:
            failures += 1
            print(f"FAIL {test} [{label}] {problem}")
    print(f"{len(runs) - failures} pass, {failures} fail")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
