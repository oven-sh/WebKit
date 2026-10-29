#!/usr/bin/env python3
"""Checks the syntax of sources of Source/JavaScriptCore/aot in seconds, without building them.

    check-syntax.py <build directory> [--define NAME[=VALUE]].. [--jobs N] [<source>.. | --changed-since <commit> | --all]

    check-syntax.py WebKitBuild/Release aot/passes/AOTSinkAllocations.cpp
    check-syntax.py WebKitBuild/Release --changed-since origin/main
    check-syntax.py WebKitBuild/Release --all --define BUN_HARDENED=1 --define ENABLE_AOT_GUARDED_PAGES=1      # what a lane with other defines would say

The build directory must have been configured and built once, so that the generated headers exist. Each source is compiled by itself with `-fsyntax-only -O0`, with the flags of a
bundle of aot/ from the build's own commands, without the precompiled header. So one error does not hide the errors of other sources, as it does under ninja, and a source that leans
on an #include of its neighbor in a unified bundle is found. A header is checked through the sources that include it: --changed-since adds those by itself.

Prints each distinct error once: file:line, in how many sources, the message. Exits with 1 if there is any.
"""
import argparse, collections, concurrent.futures, json, os, re, shlex, subprocess, sys

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
JAVASCRIPTCORE = os.path.join(ROOT, "Source", "JavaScriptCore")
FLAGS_WITH_A_VALUE = {"-o", "-MF", "-MT", "-MQ", "-include-pch", "-include"}
FLAGS_TO_DROP = re.compile(r"^(-c|-MD|-MMD|-O[0-3sz]|-g\S*|-Winvalid-pch|-fpch-\S+|-fdiagnostics-color\S*|-flto\S*|-Xclang)$")
ERROR = re.compile(r"^([^:\s]+):(\d+):\d+: (?:fatal )?error: (.*)$")


def compile_commands(build):
    path = os.path.join(build, "compile_commands.json")
    if os.path.exists(path):
        return json.load(open(path))
    return json.loads(subprocess.run(["ninja", "-t", "compdb"], cwd=build, capture_output=True, text=True, check=True).stdout)


def find_template(commands, is_test):
    wanted = "/aot/tests/" if is_test else "aot"
    for entry in commands:
        name = entry["file"]
        if name.endswith(".cpp") and ("UnifiedSource-aot" in name or "/JavaScriptCore/aot/" in name) and (wanted in name):
            return entry
    return None


def command_for(source, template, defines):
    arguments = shlex.split(template["command"]) if "command" in template else list(template["arguments"])
    kept, skip = [], False
    for argument in arguments:
        if skip:
            skip = False
        elif argument in FLAGS_WITH_A_VALUE:
            skip = True
        elif not FLAGS_TO_DROP.match(argument) and argument != template["file"]:
            kept.append(argument)
    # A folder of aot/ that is newer than the build's configuration is in none of its include paths.
    folders = sorted({folder for folder, _, names in os.walk(os.path.join(JAVASCRIPTCORE, "aot")) if any(name.endswith((".h", ".inc", ".def")) for name in names)})
    kept += [flag for folder in folders for flag in ("-I", folder)]
    return kept + ["-include", os.path.join(JAVASCRIPTCORE, "JavaScriptCorePrefix.h"), "-DRELEASE_WITHOUT_OPTIMIZATIONS", "-fsyntax-only", "-fdiagnostics-color=never", "-ferror-limit=40", "-Wno-everything",
                   "-Werror=undefined-inline", "-Werror=undefined-internal", "-Werror=switch"] + ["-D" + define for define in defines] + [source]  # fmt: skip


def all_sources():
    return sorted(os.path.join(folder, name) for folder, _, names in os.walk(os.path.join(JAVASCRIPTCORE, "aot")) for name in names if name.endswith(".cpp"))


def sources_changed_since(commit):
    changed = subprocess.run(["git", "-C", ROOT, "diff", "--name-only", commit, "--", "Source/JavaScriptCore/aot"], capture_output=True, text=True, check=True).stdout.split()
    changed += subprocess.run(["git", "-C", ROOT, "ls-files", "--others", "--exclude-standard", "--", "Source/JavaScriptCore/aot"], capture_output=True, text=True, check=True).stdout.split()
    sources = {os.path.join(ROOT, path) for path in changed if path.endswith(".cpp") and os.path.exists(os.path.join(ROOT, path))}
    headers = {os.path.basename(path) for path in changed if path.endswith((".h", ".inc", ".def"))}
    if headers:
        includes = re.compile(r'#\s*include\s+["<](?:JavaScriptCore/)?(%s)[">]' % "|".join(map(re.escape, headers)))
        sources |= {source for source in all_sources() if includes.search(open(source, errors="replace").read())}
    return sorted(sources)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build")
    parser.add_argument("sources", nargs="*")
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    parser.add_argument("--changed-since")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--root", help="the checkout that the build directory was configured for, if it is not the one this script lies in")
    options = parser.parse_intermixed_args()
    if options.root:
        global ROOT, JAVASCRIPTCORE
        ROOT = os.path.realpath(options.root)
        JAVASCRIPTCORE = os.path.join(ROOT, "Source", "JavaScriptCore")

    commands = compile_commands(options.build)
    template = find_template(commands, False)
    if not template:
        sys.exit("No command of %s compiles a source of aot/: configure the build first" % options.build)
    test_template = find_template(commands, True) or template
    if options.all:
        sources = all_sources()
    elif options.changed_since:
        sources = sources_changed_since(options.changed_since)
    else:
        sources = [path if os.path.isabs(path) else next((c for c in (os.path.abspath(path), os.path.join(JAVASCRIPTCORE, path)) if os.path.exists(c)), path) for path in options.sources]
    if not sources:
        sys.exit("Nothing to check")

    def check(source):
        chosen = test_template if "/aot/tests/" in source else template
        result = subprocess.run(command_for(source, chosen, options.define), cwd=chosen["directory"], capture_output=True, text=True)
        errors = [(os.path.relpath(os.path.realpath(os.path.join(chosen["directory"], m.group(1))), ROOT), int(m.group(2)), m.group(3)) for m in map(ERROR.match, result.stderr.split("\n")) if m]
        if result.returncode and not errors:
            errors = [(os.path.relpath(source, ROOT), 0, (result.stderr.strip().split("\n") or ["exit %d" % result.returncode])[-1][:200])]
        return source, errors

    found = collections.defaultdict(set)
    with concurrent.futures.ThreadPoolExecutor(options.jobs) as pool:
        for source, errors in pool.map(check, sources):
            for error in errors:
                found[error].add(source)
    for (path, line, message), where in sorted(found.items()):
        print("%s:%d x%d %s" % (path, line, len(where), message[:260]))
    print("%d sources checked, %d distinct errors" % (len(sources), len(found)))
    sys.exit(1 if found else 0)


if __name__ == "__main__":
    main()
