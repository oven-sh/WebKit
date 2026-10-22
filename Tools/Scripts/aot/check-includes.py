#!/usr/bin/env python3
"""Does every file that a branch changes include what it uses? About a minute per pass.

    check-includes.py <build directory> <base commit> [--headers] [--gate-off] [--jobs N]
    check-includes.py <build directory> --aot [--headers] [--gate-off] [--jobs N]

A precompiled header and unified sources hide a missing include: something earlier in the same translation unit happened to
provide it. The builds for Windows have neither the same header nor the same bundles, stop at the first few errors, and take
ten minutes a round. This finds the same errors natively, all at once.

Every .cpp outside aot/ whose working copy differs from <base commit>, committed or not, is checked for syntax on its own, with
the flags of the build in <build directory> (which needs a compile_commands.json) minus the precompiled header.

--aot       instead of what changed outside aot/, every file of aot/, changed or not, each source with its own flags. CMake
            compiles each of them on its own and has no precompiled header: what this finds is red on every platform there.
--headers   instead of the sources, the headers: each is included on its own after config.h: is it self-contained?
--gate-off  with ENABLE(AOT) forced off, as on the platforms without a back end. (Forcing it on the
            command line means something here because no precompiled header was built with the other value.)

Run all the combinations before pushing. Errors on lines that the branch adds are the branch's. The rest are shown apart: some
of upstream's headers are not self-contained either.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
LIBRARIES = ("JavaScriptCore", "WTF", "bmalloc")
# Another header includes these below its own declarations, which they name. So do the generated headers.
INCLUDED_BELOW_DECLARATIONS = ("aot/builtins/AOTBuiltinTableGuards.h", "aot/runtime/AOTHeapCellVisits.h")


def git(*arguments):
    return subprocess.run(["git", "-C", ROOT, *arguments], capture_output=True, text=True, check=True).stdout


def path_of(entry, file):
    return os.path.realpath(os.path.join(entry["directory"], file))


def flags_of(entry):
    """The compiler and its flags, without what names the input, the output or the precompiled header."""
    words = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    kept = []
    i = 0
    while i < len(words):
        word = words[i]
        following = words[i + 1] if i + 1 < len(words) else ""
        if word in ("-o", "-MF", "-MT", "-include-pch"):
            i += 2
        elif word in ("-c", "-MD", "-Winvalid-pch") or word == entry["file"] or (word.endswith(".cpp") and path_of(entry, word) == path_of(entry, entry["file"])):
            i += 1
        elif word == "-Xclang" and (following in ("-include-pch", "-include") or following.endswith(".pch") or "cmake_pch" in following):
            i += 2
        elif word == "-include" and "cmake_pch" in following:
            i += 2
        else:
            kept.append(word)
            i += 1
    return kept, entry["directory"]


def lines_added(base, path):
    added = set()
    for match in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", git("diff", "-U0", base, "--", path), re.M):
        first, count = int(match.group(1)), int(match.group(2) or 1)
        added.update(range(first, first + count))
    return added


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_directory")
    parser.add_argument("base", nargs="?")
    parser.add_argument("--aot", action="store_true")
    parser.add_argument("--headers", action="store_true")
    parser.add_argument("--gate-off", action="store_true")
    parser.add_argument("--jobs", type=int, default=6)
    arguments = parser.parse_args()
    if bool(arguments.base) == arguments.aot:
        parser.error("either a base commit or --aot")

    with open(os.path.join(arguments.build_directory, "compile_commands.json")) as file:
        commands = json.load(file)
    flags = {library: flags_of(next(e for e in commands if f"/{library}/" in e["file"] and (library != "JavaScriptCore" or "UnifiedSource" in e["file"]))) for library in LIBRARIES}
    entries = {path_of(e, e["file"]): e for e in commands}
    if arguments.aot:
        # A test may include every folder of aot/.
        flags["JavaScriptCore"] = flags_of(next(e for e in commands if "/aot/tests/" in e["file"]))
    # (Without -fno-color-diagnostics the errors below would not be recognized, and the check would always pass.)
    extra = ["-fsyntax-only", "-ferror-limit=8", "-fno-color-diagnostics"]
    if arguments.gate_off:
        extra += ["-DENABLE_AOT=0"]

    suffix = ".h" if arguments.headers else ".cpp"
    folders = [f"Source/{library}" for library in LIBRARIES]
    untracked = set()
    if arguments.aot:
        found = [os.path.relpath(os.path.join(folder, name), ROOT) for folder, _, names in os.walk(os.path.join(ROOT, "Source/JavaScriptCore/aot")) for name in names]
        files = sorted(f for f in found if f.endswith(suffix) and (entries.get(os.path.realpath(os.path.join(ROOT, f))) or arguments.headers))
        files = [f for f in files if not arguments.headers or ("/generated/" not in f and not f.endswith(INCLUDED_BELOW_DECLARATIONS))]
    else:
        untracked = set(git("ls-files", "--others", "--exclude-standard", "--", *folders).split())
        changed = git("diff", "--name-only", arguments.base, "--", *folders).split() + sorted(untracked)
        files = [f for f in changed if f.endswith(suffix) and "/aot/" not in f and os.path.exists(os.path.join(ROOT, f))]
    scratch = tempfile.mkdtemp(prefix="check-includes-")

    def check(path):
        compiler, directory = flags[path.split("/")[1]]
        source = os.path.join(ROOT, path)
        if os.path.realpath(source) in entries:
            compiler, directory = flags_of(entries[os.path.realpath(source)])
        if arguments.headers:
            source = os.path.join(scratch, path.replace("/", "_") + ".cpp")
            with open(source, "w") as file:
                file.write(f'#include "config.h"\n#include "{os.path.join(ROOT, path)}"\n')
        result = subprocess.run(compiler + extra + [source], cwd=directory, capture_output=True, text=True)
        errors = [line for line in result.stderr.split("\n") if " error: " in line]
        if result.returncode and not errors:
            errors = [f"{os.path.join(ROOT, path)}:0:0: error: exit {result.returncode} without an error that could be read: {result.stderr[:100]!r}"]
        return path, errors

    ours, others = [], []
    with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
        for path, errors in pool.map(check, files):
            added = lines_added(arguments.base, path) if errors and path not in untracked and not arguments.aot else set()
            for error in errors:
                match = re.match(r"(.*?):(\d+):\d+: (?:fatal )?error: (.*)", error)
                if not match:
                    continue
                where, line, message = match.group(1), int(match.group(2)), match.group(3)[:120]
                entry = f"{os.path.relpath(where, ROOT) if os.path.isabs(where) else where}:{line}: {message}"
                (ours if arguments.aot or (where.endswith(path) and (path in untracked or line in added)) else others).append(entry)

    print(f"{len(files)} files checked on their own")
    print(f"{len(ours)} errors on lines that the branch adds")
    for entry in ours:
        print("  " + entry)
    print(f"{len(others)} errors elsewhere; the first of each file:")
    seen = set()
    for entry in others:
        file = entry.split(":")[0]
        if file not in seen:
            seen.add(file)
            print("  " + entry)
    return 1 if ours else 0


if __name__ == "__main__":
    sys.exit(main())
