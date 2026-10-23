#!/usr/bin/env python3
"""Check that the unified source bundles of aot/ compile. Takes two to five minutes.

    check-unified-sources.py <build directory> [--write] [--jobs N] [directory ..]

CMake compiles aot/ as unified sources (AOT_GENERATE_UNIFIED_SOURCES in aot/AOT.cmake). Bun's own build compiles each source
separately, so it does not notice when two sources in a directory define the same file-local name (a static function, a type
in an anonymous namespace) or when a `using namespace` leaks into a header that a later source includes. In a bundle those are
compile errors, on every platform.

This syntax-checks every bundle, using the flags of an aot/ source from <build directory>/compile_commands.json (CMake's or
Bun's) without the precompiled header. It also checks each whole group as a single translation unit, because adding a source
shifts every bundle after it.

To fix a collision, rename one of the two, or list the source in JavaScriptCore_AOT_SEPARATELY_UNIFIED_SOURCES (those are
bundled with each other), or, if it collides there too, in JavaScriptCore_AOT_NON_UNIFIED_SOURCES.

--write        update the two lists in aot/AOT.cmake and repeat until everything compiles
directory ..   only check these directories of aot/ (passes, codegen, ...)
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
JAVASCRIPTCORE = os.path.join(ROOT, "Source", "JavaScriptCore")
CMAKE_LISTS = os.path.join(JAVASCRIPTCORE, "aot", "AOT.cmake")
SEPARATELY_UNIFIED = "JavaScriptCore_AOT_SEPARATELY_UNIFIED_SOURCES"
NON_UNIFIED = "JavaScriptCore_AOT_NON_UNIFIED_SOURCES"
COMPILED_FOR_SIZE = "JavaScriptCore_AOT_SOURCES_COMPILED_FOR_SIZE"


def flags_of(entry):
    """The compiler and its flags, without the input, the output and the precompiled header."""
    words = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    kept = []
    i = 0
    while i < len(words):
        word = words[i]
        following = words[i + 1] if i + 1 < len(words) else ""
        if word in ("-o", "-MF", "-MT", "-include-pch"):
            i += 2
        elif word in ("-c", "-MD", "-Winvalid-pch") or word.endswith(".cpp"):
            i += 1
        elif word == "-Xclang" and (following in ("-include-pch", "-include") or following.endswith(".pch") or "cmake_pch" in following):
            i += 2
        elif word == "-include" and "cmake_pch" in following:
            i += 2
        else:
            kept.append(word)
            i += 1
    return kept, entry["directory"]


def read(path):
    with open(path) as file:
        return file.read()


def cmake_list(text, name):
    return re.search(r"^set\(" + name + r"\n(.*?)^\)", text, re.M | re.S).group(1).split()


def library_sources():
    return [line.split()[0] for line in read(os.path.join(JAVASCRIPTCORE, "aot", "Sources.txt")).splitlines() if line.strip()]


def translation_units(directories):
    """The bundles AOT_GENERATE_UNIFIED_SOURCES generates, plus each whole group. Name -> sources."""
    text = read(CMAKE_LISTS)
    separate, non_unified, for_size = (set(cmake_list(text, name)) for name in (SEPARATELY_UNIFIED, NON_UNIFIED, COMPILED_FOR_SIZE))
    bundle_size = int(re.search(r"^    set\(bundleSize (\d+)\)", text, re.M).group(1))
    groups = collections.OrderedDict()
    for path in library_sources():
        directory = path.split("/")[1]
        if path not in non_unified and (not directories or directory in directories):
            groups.setdefault(directory + ("-for-size" if path in for_size else "") + ("-separate" if path in separate else ""), []).append(path)
    units = collections.OrderedDict()
    for group, sources in groups.items():
        if len(sources) > bundle_size:
            units[group] = sources
        for start in range(0, len(sources), bundle_size):
            units[f"{group}-{start // bundle_size + 1}"] = sources[start:start + bundle_size]
    return units


def colliding_sources(unit, sources, output):
    """Yields (source to take out of the unit, error). Only the first error in each source counts: the rest may be fallout."""
    source_at_line = {line + 1: source for line, source in enumerate(sources)}
    errors = []
    includer = None
    for line in output.split("\n"):
        match = re.match(r"In file included from " + re.escape(unit) + r":(\d+):", line)
        if match:
            includer = source_at_line[int(match.group(1))]
            continue
        match = re.match(r"(.*?):(\d+):\d+: (fatal error|error|note): (.*)", line)
        if not match:
            continue
        where = os.path.relpath(os.path.realpath(match.group(1)), JAVASCRIPTCORE) if os.path.isabs(match.group(1)) else match.group(1)
        if match.group(3) == "note":
            if errors:
                errors[-1][2].append(where)
        else:
            errors.append((where if where in sources else includer, f"{where}:{match.group(2)}: {match.group(4)[:140]}", []))
    seen = set()
    for source, message, notes in errors:
        if source is None or source in seen:
            continue
        seen.add(source)
        # An ambiguous name in a header is the fault of the earlier source that declared the other candidate.
        earlier = [note for note in notes if note in sources and note != source]
        yield (earlier[0] if re.search(r"reference to .* is ambiguous", message) and earlier else source), message


def updated_lists(colliding):
    """A source moves from the regular bundles to the separate ones, and from those to no bundle at all."""
    text = read(CMAKE_LISTS)
    existing = set(library_sources())
    separate, non_unified = set(cmake_list(text, SEPARATELY_UNIFIED)), set(cmake_list(text, NON_UNIFIED))
    return {SEPARATELY_UNIFIED: (separate ^ colliding) & existing, NON_UNIFIED: (non_unified | (separate & colliding)) & existing}


def write_lists(lists):
    text = read(CMAKE_LISTS)
    for name, entries in lists.items():
        body = "".join(f"    {entry}\n" for entry in sorted(entries))
        text = re.sub(r"(^set\(" + name + r"\n).*?(^\))", lambda match: match.group(1) + body + match.group(2), text, count=1, flags=re.M | re.S)
    with open(CMAKE_LISTS, "w") as file:
        file.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_directory")
    parser.add_argument("directories", nargs="*")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--jobs", type=int, default=6)
    arguments = parser.parse_args()

    commands = json.loads(read(os.path.join(arguments.build_directory, "compile_commands.json")))
    compiler, directory = flags_of(next(e for e in commands if "UnifiedSource-aot-" in e["file"] or re.search(r"/aot/(?!tests/)\w+/AOT\w+(?<!TestCommands)\.cpp$", e["file"])))
    # Without -fno-color-diagnostics the errors would not be recognized and the check would always pass.
    compiler += ["-fsyntax-only", "-ferror-limit=0", "-fno-color-diagnostics", "-w", "-g0", f"-I{JAVASCRIPTCORE}"]
    scratch = tempfile.mkdtemp(prefix="check-unified-sources-")

    def check(item):
        name, sources = item
        unit = os.path.join(scratch, f"UnifiedSource-aot-{name}.cpp")
        with open(unit, "w") as file:
            file.write("".join(f'#include "{source}"\n' for source in sources))
        result = subprocess.run(compiler + [unit], cwd=directory, capture_output=True, text=True, errors="replace")
        found = list(colliding_sources(unit, sources, result.stderr))
        if result.returncode and not found:
            found = [(None, f"{name}: exit {result.returncode} with no error that could be parsed: {result.stderr[:200]!r}")]
        return found

    while True:
        units = translation_units(set(arguments.directories))
        with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
            found = [collision for collisions in pool.map(check, units.items()) for collision in collisions]
        colliding = {source for source, _ in found if source}
        print(f"{len(units)} translation units checked, {len(colliding)} sources collide", flush=True)
        for message in sorted({message for _, message in found}):
            print("  " + message)
        if not found:
            return 0
        if not arguments.write or not colliding:
            print("--write would move these in aot/AOT.cmake:")
            for source in sorted(colliding):
                print("    " + source)
            return 1
        write_lists(updated_lists(colliding))


if __name__ == "__main__":
    sys.exit(main())
