#!/usr/bin/env python3
"""Runs the engine's own tests of ahead-of-time compilation and sound types. A few minutes.

    run-tests.py <jsc> [--jobs <n>]

The tests are JSTests/stress/aot-*.js and sound-types-*.js. Each `//@ runDefault(...)` of a header is a run of its own; the runs of
one test follow each other, since they may write the same files, and different tests run beside each other. The first one
runs twice more: with every inferred type checked against the value and the B3 and Air validators on, as in the mode `aot-validate`;
and without data stubs, which is what a CPU other than ARM64 and x86-64 would compile. A `//@ run("name", ...)` in the header is one
more run, with the options of the first `runDefault` and its own.

A test fails if it prints anything or exits with anything but 0. run-javascriptcore-tests runs all of this and much more, in
the modes `aot` and `aot-validate` among the rest. This is what is quick enough to run after every change.
"""
import argparse
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STRESS = os.path.realpath(os.path.join(HERE, "..", "..", "..", "JSTests", "stress"))

def header_of(path):
    with open(path, errors="replace") as file:
        lines = file.read().split("\n")
    count = next((index for index, line in enumerate(lines) if not line.startswith("//@ ")), len(lines))
    return lines[:count]


def options_in(text):
    return re.findall(r'"(--?[^"]*)"', text)


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
    parser.add_argument("--jobs", type=int, default=4)
    arguments = parser.parse_args()
    jsc = os.path.abspath(arguments.jsc)

    tests = sorted(os.path.basename(path) for pattern in ("aot-*.js", "sound-types-*.js") for path in glob.glob(os.path.join(STRESS, pattern)))
    runs = []
    for test in tests:
        header = header_of(os.path.join(STRESS, test))
        common = options_in(" ".join(line for line in header if not line.startswith("//@ run")))
        lines = [common + options_in(line) for line in header if re.match(r"//@ run[A-Z]", line)] or [common]
        options = lines[0]
        for line in header:
            if also := re.match(r'//@ run\("([^"]+)"(.*)\)', line):
                runs.append((test, options + options_in(also.group(2)), also.group(1)))
        for index, line in enumerate(lines):
            runs.append((test, line, "as it says" if len(lines) == 1 else f"run {index + 1} of {len(lines)}"))
        if not any("$skipModes << :aot_validate" in line for line in header):
            runs.append((test, options + ["--validateAOTInferredTypes=true", "--validateGraphAtEachPhase=true", "--aotTypeCoveragePath="], "validated"))
        runs.append((test, options + ["--useAOTDataStubs=false"], "without data stubs"))

    failures = 0
    runs_of = {test: [entry for entry in runs if entry[0] == test] for test in tests}
    with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        for problems in pool.map(lambda test: [(entry, run(jsc, entry[1], test)) for entry in runs_of[test]], tests):
            for (test, options, label), problem in problems:
                if problem:
                    failures += 1
                    print(f"FAIL {test} [{label}] {problem}")
    print(f"{len(runs) - failures} pass, {failures} fail")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
