#!/usr/bin/env python3
# Prints the memory layout of every record the bytecode cache (runtime/CachedTypes.cpp) writes into a payload, in a
# form that is textually identical across targets exactly when the layouts are. The release workflow diffs this file
# between platforms: a payload encoded on one must decode on the others, so their Cached* records must agree.
#
# Usage: dump-bytecode-cache-layout.py <build dir>            (recompiles the CachedTypes.cpp translation unit with -fdump-record-layouts)
#        dump-bytecode-cache-layout.py --dump <clang output>   (parses an existing dump; for testing)

import json, os, re, shlex, subprocess, sys

def compile_command(build_dir):
    with open(os.path.join(build_dir, "compile_commands.json")) as f:
        commands = json.load(f)
    for entry in commands:
        source = entry["file"]
        if "JavaScriptCore" not in source or "UnifiedSource" not in source:
            continue
        path = source if os.path.isabs(source) else os.path.join(entry["directory"], source)
        try:
            with open(path) as f:
                if '"runtime/CachedTypes.cpp"' not in f.read():
                    continue
        except OSError:
            continue
        args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
        return entry["directory"], args
    sys.exit("no translation unit in compile_commands.json includes runtime/CachedTypes.cpp")

def record_layouts(build_dir):
    directory, args = compile_command(build_dir)
    if os.path.basename(args[0]) in ("ccache", "sccache"):
        args = args[1:]
    # The same command minus its outputs: only the front end runs, and it prints every record layout it computes.
    filtered, skip = [], False
    for arg in args:
        if skip:
            skip = False
        elif arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif arg in ("-c", "/c", "-MD", "-MMD", "/showIncludes") or arg.startswith(("-o", "/Fo", "/Fd", "-MF", "-MT")):
            pass
        else:
            filtered.append(arg)
    filtered[1:1] = ["-fsyntax-only", "-Xclang", "-fdump-record-layouts"] # ahead of any `--` (clang-cl commands end `-- <source>`)
    result = subprocess.run(filtered, cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode:
        sys.stderr.write(result.stderr)
        sys.exit("dumping record layouts failed: " + shlex.join(filtered))
    return result.stdout

def normalize(name):
    name = re.sub(r"\b(class|struct|union|enum) ", "", name)
    name = re.sub(r"\(unnamed (?:\w+ )?at [^)]*?([^/\\:)]+:\d+:\d+)\)", r"(unnamed at \1)", name)
    name = re.sub(r"\bunsigned long long\b|\bunsigned long\b", "ulong", name) # uint64_t's spelling differs (LP64 / LLP64); sizes are compared, not spellings
    name = re.sub(r"(?<!unsigned )\blong long\b|(?<![nu] )\blong\b", "long", name)
    return name.strip()

class Record:
    def __init__(self, name):
        self.name = name
        self.rows = []      # (offset, depth, text) for every row under the record, nested subobjects included
        self.members = []   # (offset, type name or None, text) for direct members and bases
        self.bases = []
        self.size = self.align = None

def parse(dump):
    records = {}
    for block in dump.split("*** Dumping AST Record Layout\n")[1:]:
        lines = block.split("\n")
        head = re.match(r"\s*0 \| (.*)$", lines[0])
        sizes = re.search(r"\[sizeof=(\d+),(?:\s*dsize=\d+,)?\s*align=(\d+)", block)
        if not head or not sizes:
            continue
        record = Record(normalize(head.group(1)))
        record.size, record.align = int(sizes.group(1)), int(sizes.group(2))
        for line in lines[1:]:
            row = re.match(r"\s*([\d:.-]+) \| ( +)(.*)$", line)
            if not row:
                continue
            offset, depth, text = row.group(1), len(row.group(2)) // 2, normalize(row.group(3))
            is_base = re.search(r" \((?:primary |virtual )?base\)(?: \(empty\))?$", text)
            type_name = re.sub(r" \((?:primary |virtual )?base\)(?: \(empty\))?$", "", text) if is_base else (re.match(r"(.+) [A-Za-z_]\w*(\[\d*\])*$", text) or [None, None])[1]
            record.rows.append((offset, depth, text))
            if depth == 1:
                record.members.append((offset, type_name, text))
                if is_base:
                    record.bases.append(type_name)
        records[record.name] = record
    return records

def template_arguments(name):
    depth, start, out = 0, None, []
    for i, c in enumerate(name):
        if c == "<":
            depth += 1
            if depth == 1:
                start = i + 1
        elif c == ">":
            depth -= 1
            if depth == 0:
                out.append(name[start:i])
        elif c == "," and depth == 1:
            out.append(name[start:i])
            start = i + 1
    return [a.strip() for a in out]

def cache_records(records):
    # Roots: everything laid out in a payload derives from CachedObject<> / VariableLengthObjectBase, plus the entry headers.
    def is_root(record, seen=()):
        if re.match(r"JSC::(\w*CacheEntry\b|CachedObject<|VariableLengthObjectBase$)", record.name):
            return True
        return any(base in records and base not in seen and is_root(records[base], seen + (record.name,)) for base in record.bases)
    # T of every CachedObject<T> / VariableLengthObject<T>: the in-memory types the cache converts from, never laid out in a payload.
    source_types = {template_arguments(r.name)[0] for r in records.values() if re.match(r"JSC::(CachedObject|VariableLengthObject)<", r.name)}
    selected, queue = {}, [r for r in records.values() if is_root(r)]
    while queue:
        record = queue.pop()
        if record.name in selected:
            continue
        selected[record.name] = record
        reached = [t for _, t, _ in record.members if t]
        # A Cached container's element type is written into the payload too (in its variable-length tail), so follow
        # template arguments -- except CachedObject<T> / VariableLengthObject<T>, whose T is the in-memory source type.
        if not re.match(r"JSC::(CachedObject|VariableLengthObject)<", record.name):
            reached += [t for t in template_arguments(record.name) if t not in source_types]
        for name in reached:
            if name.endswith(("*", "&")):
                continue
            name = normalize(name)
            if name in records and name not in selected:
                queue.append(records[name])
    return [selected[name] for name in sorted(selected)]

def main(argv):
    dump = open(argv[2]).read() if argv[1] == "--dump" else record_layouts(argv[1])
    records = parse(dump)
    selected = cache_records(records)
    if len(selected) < 50:
        sys.exit("found only %d bytecode cache records; the dump or the selection is broken" % len(selected))
    out = []
    for record in selected:
        out.append("%s size=%d align=%d" % (record.name, record.size, record.align))
        for offset, depth, text in record.rows:
            out.append("  %s%s | %s" % ("  " * (depth - 1), offset, text))
    print("\n".join(out))

if __name__ == "__main__":
    main(sys.argv)
