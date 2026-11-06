#!/usr/bin/env python3
"""check_sources_alone.py COMPILE_COMMANDS.JSON [OPTION...]: every source of python/ compiles by itself.

The sources are compiled in bundles (unified sources), and all of python/ is one sequence of bundles, in the order of the directories' names: a source compiles with whatever
those before it in its bundle have included, and one source more anywhere moves all the later ones to other places. So a source that does not name what it uses
compiles until somebody adds a file. This takes the command for one of the bundles from the compile_commands.json of a build that has run (of Bun, or CMake's) and
runs it for each source of ../../Sources.txt instead, with -fsyntax-only and the options given (-DPYTHON_HAS_CAPI=0: what a system without the C API compiles). Prints what
fails. A quarter of a minute on 16 cores."""
import concurrent.futures, json, os, re, shlex, subprocess, sys

entry = next(entry for entry in json.load(open(sys.argv[1])) if re.search(r"UnifiedSource-python-\d+\.cpp$", entry["file"]))
arguments = entry.get("arguments") or shlex.split(entry["command"])
output = arguments.index("-o")
arguments = [argument for i, argument in enumerate(arguments) if i not in (output, output + 1) and argument not in ("-c", entry["file"])]
# The tree that the build compiles, which need not be this one.
root = next(argument[2:] for argument in arguments if re.fullmatch(r"-I.*/Source/JavaScriptCore", argument))
sources = [line.split("@")[0].strip() for line in open(f"{root}/Sources.txt") if line.startswith("python/")]


def check(source):
    result = subprocess.run([*arguments, *sys.argv[2:], "-fsyntax-only", f"{root}/{source}"], cwd=entry["directory"], capture_output=True, text=True)
    return source, result.returncode, result.stderr


failed = 0
with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
    for source, status, errors in pool.map(check, sources):
        if status:
            failed += 1
            print(f"== {source}", *[line for line in errors.splitlines() if "error:" in line][:5], sep="\n")
print(f"{len(sources)} sources, {failed} do not compile by themselves")
sys.exit(bool(failed))
