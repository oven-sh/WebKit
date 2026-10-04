#!/usr/bin/env python3
"""type-coverage.py <coverage file> [options]

Reports how much of a program the ahead-of-time compiler specialized, like a code coverage report: an operation is covered
if it compiled to inline code. The coverage file is written by the compiler with --aotTypeCoveragePath=<file>.

Every operation is one of:
  inline    no call on the usual path: a typed field, a direct or inlined call, arithmetic on known numbers, ...
  stub      calls a shared stub that has an inline cache or a fast path
  runtime   calls into the C++ runtime

An operation that was compiled several times (inlined, or in both copies of a split loop) counts once, as its worst copy;
the generic copy of a split loop counts only where there is no other. A function compiled both for calls and for `new` counts
once, each operation as the better of the two, since one of them usually never runs.

Every opcode counts. Those that no kind lists (moves, jumps, returns, checks) are "other"; they have nothing to specialize, so
the row "all but other" and --fail-under leave them out.

The last table checks the report against the bytecode: how many instructions of each kind the compiled functions have, and how
many of them the compiler reported. It reports all of them: an instruction that is not in its graph (a move, or code that is
unreachable or folded) has the outcome not-in-graph.

Options:
  --source-map <file>      map positions back to the original sources; may be repeated, one per module
  --reasons <file>         reasons.txt of the type table: names the reasons why an access has no type
  --lcov <file>            write an lcov tracefile (a line is hit if everything on it is inline; each operation is a branch)
  --by <what>              also break down by: reason, property, function, opcode, outcome (may be repeated)
  --kind <kinds>           restrict breakdowns to these kinds, comma separated (reads, writes, elements, calls, arithmetic,
                           comparisons, conditions, iteration, allocation, variables, exceptions, other)
  --explain <file>[:line]  list the operations of a file or a line, with what each compiled to and why
  --top <n>                rows per table (default 25)
  --fail-under <percent>   exit with status 1 if less than this is inline
"""
import bisect
import collections
import json
import os
import re
import sys

KINDS = {
    "reads": "get_by_id get_by_id_direct get_by_id_with_this try_get_by_id in_by_id get_length get_private_name",
    "writes": "put_by_id put_by_id_with_this put_private_name define_data_property del_by_id put_getter_by_id put_setter_by_id "
              "put_getter_setter_by_id put_getter_by_val put_setter_by_val define_accessor_property put_internal_field set_function_name",
    "elements": "get_by_val put_by_val put_by_val_direct in_by_val del_by_val get_by_val_with_this get_argument_by_val "
                "enumerator_put_by_val",
    "calls": "call call_ignore_result construct tail_call call_varargs construct_varargs tail_call_varargs call_direct_eval "
             "tail_call_forward_arguments super_construct super_construct_varargs",
    "arithmetic": "add sub mul div mod pow negate inc dec bitand bitor bitxor bitnot lshift rshift urshift unsigned to_number "
                  "to_numeric to_string to_primitive to_property_key to_property_key_or_number strcat to_object",
    "comparisons": "eq neq stricteq nstricteq less lesseq greater greatereq below beloweq jeq jneq jstricteq jnstricteq jless "
                   "jlesseq jgreater jgreatereq jnless jnlesseq jngreater jngreatereq jbelow jbeloweq instanceof typeof "
                   "eq_null neq_null jeq_null jneq_null jundefined_or_null jnundefined_or_null is_callable is_object "
                   "typeof_is_object typeof_is_function typeof_is_undefined is_undefined_or_null is_cell_with_type "
                   "is_number is_boolean is_big_int is_empty is_constructor jeq_ptr jneq_ptr",
    "conditions": "jtrue jfalse not to_boolean switch_imm switch_char switch_string",
    "iteration": "iterator_open iterator_next get_property_enumerator enumerator_next enumerator_get_by_val spread",
    "allocation": "new_object create_this new_array new_array_with_size new_array_with_spread new_array_buffer new_func "
                  "new_func_exp new_generator_func new_generator_func_exp new_async_func new_async_func_exp new_reg_exp "
                  "new_reg_exp_shared new_async_generator_func new_async_generator_func_exp "
                  "create_lexical_environment create_rest create_direct_arguments create_scoped_arguments "
                  "create_cloned_arguments create_generator create_promise create_async_generator new_promise new_generator",
    "variables": "get_from_scope put_to_scope resolve_scope to_this get_argument argument_count get_scope get_parent_scope "
                 "get_internal_field resolve_and_get_from_scope",
    "exceptions": "throw throw_static_error catch",
}
KIND_OF = collections.defaultdict(lambda: "other", {"op_" + name: kind for kind, names in KINDS.items() for name in names.split()})
ALL_KINDS = list(KINDS) + ["other"]
LEVELS = ("runtime", "stub", "inline")
IN_GENERIC_COPY, RARELY_EXECUTED, IN_LOOP, INLINED, ELIDED, HAS_TYPE_TAG, NOT_IN_GRAPH = 1, 2, 4, 8, 16, 32, 64
KNOWN_CALLEE = ("direct-call", "direct-construct", "inlined-call", "inlined-construct", "inlined-closure", "inlined-builtin", "inlined-call-with-callback", "lowered-builtin")


def operations_with_fast_path():
    """Operations whose table entry is a stub that handles the common case and only then calls the C++ function."""
    header = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "Source", "JavaScriptCore", "aot", "AOTStubs.h")
    text = open(header).read()
    names = set()
    for macro, pattern, prefix in (("FOR_EACH_AOT_OPERATION_WITH_FRONT_END", r"v\((\w+)\)", "operationAOT"), ("FOR_EACH_AOT_OPERATION_BEHIND_HELPER", r"v\(\w+, (\w+)\)", "")):
        body = re.search(r"#define %s\(v\) \\\n((?:.*\\\n)*)" % macro, text).group(1)
        names.update(prefix + name for name in re.findall(pattern, body))
    return names


HAS_FAST_PATH = operations_with_fast_path()


def level_of(kind, flags, outcomes):
    if flags & (ELIDED | NOT_IN_GRAPH):
        return 2
    always = [o[6:] for o in outcomes if o.startswith("calls:")]
    if kind == "calls" and any(o.split(":")[0] in KNOWN_CALLEE for o in outcomes):
        return 2
    operations = [name for name in always if name.startswith("operation")]
    if operations and all(name in HAS_FAST_PATH for name in operations):
        return 1
    if any(name.startswith(("operation", "Operation", "PlainOperation")) for name in always):
        return 0
    return 1 if always or kind == "calls" else 2


def is_typed(outcomes):
    return any(o.startswith("typed-") for o in outcomes)


class SourceMap:
    DIGITS = {c: i for i, c in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")}

    def __init__(self, path):
        data = json.load(open(path))
        root = os.path.dirname(os.path.abspath(path))
        self.sources = [os.path.normpath(os.path.join(root, data.get("sourceRoot") or "", s)) for s in data["sources"]]
        self.lines = []
        source = line = column = 0
        for text in data["mappings"].split(";"):
            generated = 0
            columns, targets = [], []
            for segment in text.split(","):
                if not segment:
                    continue
                fields, value, shift = [], 0, 0
                for c in segment:
                    digit = self.DIGITS[c]
                    value |= (digit & 31) << shift
                    shift += 5
                    if not digit & 32:
                        fields.append(-(value >> 1) if value & 1 else value >> 1)
                        value = shift = 0
                generated += fields[0]
                if len(fields) >= 4:
                    source += fields[1]
                    line += fields[2]
                    column += fields[3]
                    columns.append(generated)
                    targets.append((source, line, column))
            self.lines.append((columns, targets))

    def original(self, line, column):
        if line > len(self.lines):
            return None
        columns, targets = self.lines[line - 1]
        i = bisect.bisect_right(columns, column - 1) - 1
        if i < 0:
            return None
        source, line, column = targets[i]
        return self.sources[source], line + 1, column + 1


def percent(part, whole):
    return "%5.1f%%" % (100.0 * part / whole) if whole else "     -"


def table(title, rows, top, header="operations   inline     stub  runtime    typed"):
    if not rows:
        return
    width = max([len(title)] + [len(name) for name, _ in rows[:top]])
    width = min(width, 90)
    print("\n%-*s  %s" % (width, title, header))
    for name, c in rows[:top]:
        n = sum(c[:3])
        print("%-*s  %10s  %s  %s  %s  %s" % (width, name[-width:], format(n, ","), percent(c[2], n), percent(c[1], n), percent(c[0], n), percent(c[3], n)))
    if len(rows) > top:
        print("... and %d more" % (len(rows) - top))


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    path, maps, reasons_path, lcov, by, kinds, explain, top, fail_under = None, [], None, None, [], None, None, 25, None
    while args:
        a = args.pop(0)
        if a == "--source-map":
            maps.append(args.pop(0))
        elif a == "--reasons":
            reasons_path = args.pop(0)
        elif a == "--lcov":
            lcov = args.pop(0)
        elif a == "--by":
            by.append(args.pop(0))
        elif a == "--kind":
            kinds = set(args.pop(0).split(","))
        elif a == "--explain":
            explain = args.pop(0)
        elif a == "--top":
            top = int(args.pop(0))
        elif a == "--fail-under":
            fail_under = float(args.pop(0))
        else:
            path = a

    reasons = {}
    if reasons_path:
        for line in open(reasons_path):
            number, _, text = line.rstrip("\n").partition("\t")
            reasons[number] = text

    modules, raw, in_bytecode, unreached = {}, collections.defaultdict(list), {}, None
    for line in open(path, errors="replace"):
        f = line.rstrip("\n").split("\t")
        if f[0] == "M":
            modules[f[1]] = f[2]
        elif f[0] == "T":
            in_bytecode[f[1]] = (int(f[2]), int(f[3]))
        elif f[0] == "U":
            unreached = (int(f[1]), int(f[2]))
        elif f[0] == "O":
            raw[(f[1], int(f[10]), int(f[11]), int(f[12]))].append((int(f[5]), f[6], f[7], f[8], f[9].split() if f[9] else [], int(f[2]), int(f[3]), f[4]))

    by_name = {os.path.basename(m)[:-4]: m for m in maps}
    map_of = {}
    for module, url in modules.items():
        found = by_name.get(os.path.basename(url)) or (maps[0] if len(maps) == 1 and len(modules) == 1 else None)
        if found:
            map_of[module] = SourceMap(found)

    reported = collections.Counter()
    variants = collections.defaultdict(list)
    ordinals = collections.Counter()
    for (module, start, function_kind, offset), copies in sorted(raw.items()):
        opcode = copies[0][7]
        kind = KIND_OF[opcode]
        reported[opcode] += 1
        if any(not c[0] & IN_GENERIC_COPY for c in copies):
            copies = [c for c in copies if not c[0] & IN_GENERIC_COPY]
        worst = min(copies, key=lambda c: (level_of(kind, c[0], c[4]), is_typed(c[4])))
        place = (module, start, function_kind, worst[5], worst[6], opcode)
        ordinals[place] += 1
        variants[(module, start, function_kind >> 1, worst[5], worst[6], opcode, ordinals[place])].append((worst, len(copies)))

    operations = []
    for (module, start, _, line, column, opcode, _), found in variants.items():
        kind = KIND_OF[opcode]
        best, copies = max(found, key=lambda v: (level_of(kind, v[0][0], v[0][4]), is_typed(v[0][4])))
        flags, reason, name, function, outcomes = best[:5]
        copies = [None] * copies
        where = (modules.get(module, module), line, column)
        if module in map_of:
            where = map_of[module].original(line, column) or ("(no original position)", 0, 0)
        operations.append((where, opcode, kind, level_of(kind, flags, outcomes), is_typed(outcomes), flags, reason, name, function, outcomes, len(copies)))

    def tally(key, chosen=None):
        counts = collections.defaultdict(lambda: [0, 0, 0, 0])
        for o in operations:
            if chosen and o[2] not in chosen:
                continue
            k = key(o)
            if k is None:
                continue
            counts[k][o[3]] += 1
            counts[k][3] += o[4]
        return counts

    def reason_of(o):
        if o[4]:
            return "(typed)"
        if o[6]:
            return reasons.get(o[6], "reason " + o[6])
        return "(has a type that was not used)" if o[5] & HAS_TYPE_TAG else "(no type given)"

    if explain:
        want, _, want_line = explain.partition(":")
        for o in sorted(operations):
            (file, line, column) = o[0]
            if not file.endswith(want) or (want_line and int(want_line) != line):
                continue
            print("%s:%d:%d  %-18s %-7s %s" % (os.path.basename(file), line, column, o[1][3:] + (" ." + o[7] if o[7] else ""), LEVELS[o[3]], " ".join(o[9]) or ("elided" if o[5] & ELIDED else "-")))
            if o[2] in ("reads", "writes") and not o[4]:
                print("%*s not typed: %s" % (len(os.path.basename(file)) + 8, "", reason_of(o)))
        return 0

    common = os.path.dirname(os.path.commonpath([os.path.join(o[0][0], "x") for o in operations if o[0][0].startswith("/")] or ["/x"]))
    if os.path.isfile(common) or len({o[0][0] for o in operations}) == 1:
        common = os.path.dirname(common)
    short = lambda file: os.path.relpath(file, common) if file.startswith("/") and common != "/" else file

    counts = tally(lambda o: o[2])
    rows = [(k, counts[k]) for k in ALL_KINDS if k in counts]
    total = [sum(c[i] for k, c in rows if k != "other") for i in range(4)]
    everything = [sum(c[i] for _, c in rows) for i in range(4)]
    table("Kind of operation", rows + [("all but other", total), ("all", everything)], 99)
    files = tally(lambda o: short(o[0][0]), kinds)
    table("File" + (" (" + ", ".join(sorted(kinds)) + ")" if kinds else ""), sorted(files.items(), key=lambda kv: -(kv[1][0] + kv[1][1])), top)

    keys = {
        "reason": (reason_of, kinds or {"reads", "writes"}),
        "property": (lambda o: o[7] or None, kinds or {"reads", "writes"}),
        "function": (lambda o: o[8] or "(top level)", kinds),
        "opcode": (lambda o: o[1][3:], kinds),
    }
    for what in by:
        if what == "outcome":
            seen = collections.Counter(x for o in operations if not kinds or o[2] in kinds for x in (o[9] or ["(nothing)"]))
            print("\nOutcome")
            for name, n in seen.most_common(top):
                print("%10s  %s" % (format(n, ","), name))
            continue
        key, chosen = keys[what]
        title = what.capitalize() + (" (" + ", ".join(sorted(chosen)) + ")" if chosen else "")
        table(title, sorted(tally(key, chosen).items(), key=lambda kv: -(kv[1][0] + kv[1][1])), top)

    if lcov:
        per_file = collections.defaultdict(lambda: collections.defaultdict(list))
        for o in operations:
            if o[0][1]:
                per_file[o[0][0]][o[0][1]].append(o)
        with open(lcov, "w") as out:
            for file in sorted(per_file):
                out.write("TN:\nSF:%s\n" % file)
                lines = per_file[file]
                found = hit = 0
                for line in sorted(lines):
                    for i, o in enumerate(sorted(lines[line])):
                        out.write("BRDA:%d,0,%d,%s\n" % (line, i, "1" if o[3] == 2 else "0"))
                        found += 1
                        hit += o[3] == 2
                out.write("BRF:%d\nBRH:%d\n" % (found, hit))
                for line in sorted(lines):
                    out.write("DA:%d,%d\n" % (line, all(o[3] == 2 for o in lines[line])))
                out.write("LF:%d\nLH:%d\nend_of_record\n" % (len(lines), sum(all(o[3] == 2 for o in ops) for ops in lines.values())))
        print("\nwrote", lcov)

    if in_bytecode:
        kinds_in_bytecode = collections.defaultdict(lambda: [0, 0, 0])
        for opcode, (positioned, unpositioned) in in_bytecode.items():
            k = kinds_in_bytecode[KIND_OF[opcode]]
            k[0] += positioned
            k[1] += reported[opcode]
            k[2] += unpositioned
        print("\n%-18s %12s %12s %10s %22s" % ("Completeness", "in bytecode", "reported", "", "in class field code"))
        for kind in ALL_KINDS + ["all"]:
            k = kinds_in_bytecode[kind] if kind != "all" else [sum(v[i] for v in kinds_in_bytecode.values()) for i in range(3)]
            print("%-18s %12s %12s %10s %22s" % (kind, format(k[0], ","), format(k[1], ","), percent(k[1], k[0]), format(k[2], ",")))
    if unreached:
        print("%s functions (%s bytes of bytecode) were not compiled: nothing reaches them" % (format(unreached[0], ","), format(unreached[1], ",")))

    n = sum(total[:3])
    if fail_under is not None and n and 100.0 * total[2] / n < fail_under:
        print("\n%.1f%% is inline, less than %.1f%%" % (100.0 * total[2] / n, fail_under))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
