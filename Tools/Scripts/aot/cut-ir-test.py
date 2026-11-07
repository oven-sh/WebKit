#!/usr/bin/env python3
"""Cuts an IR text test (aot/tests/**/*.ir) down to the program's own files and what they reach in the library.

    cut-ir-test.py --testaot <testaot> [--write] [--jobs <n>] [--bar <lines>] <file or folder> ..

A test that was made from a source holds what the front end wrote for the whole program, and the library rides along in it: every function
that the host may call is a root of its own, so Link calls none of them unreached: 25,000 lines and more. This tool takes those roots away:

  kept whole     the records of the program's own modules, `entry`, and the library functions that Link still reaches from there
  kept as a head a library function that a kept record names and nothing reaches: its first block and `Unreachable`
  gone           every other record, and each `runtimefunction` line but the few on which a print or an answer hangs

The reader wants the ids of a list in a row, so what stands behind a gap is numbered anew. The program's own records come first in every
list, so theirs stay; where an id that a directive names does change, the directive changes with it.

THE PROOF, for each file, by the testaot that is handed over (jsc beside it), in each way in which the tests are run: as they are,
--mechanisms=none (the closed ways call library functions that the typed forms never reach), --option=check=Validate:
  1. the test passes as it stands (else it is left alone)
  2. every RUN of the cut test prints what it printed: line for line, the ids of the control's print numbered as the cut numbers them.
     A RUN that names no function prints the whole program (`link lookups`, `bytes-by-form`): each of its lines is one of the control's, as
     often, whatever the order, numbers aside.
     A RUN that counts answers (`run-in-shell`, "103 of 103 of 103") prints the answers themselves for this: see with_answers_shown().
     Where a print differs, the functions that the host may call are looked through for the few that must stay roots; if the cut is still
     fat (--fat), the bodies that are left are looked through for those that it hangs on.
  3. the cut test passes
  4. the red control: with the body of a function that its RUN lines name taken out, the test fails (of a test whose RUN lines name none:
     the body of the entry module). Where it stays green the uncut test is asked too. A test whose checks ask for no line of a body
     (CHECK-LABEL or CHECK-NOT alone) cannot turn red so, cut or uncut: it is cut, and its line says NO RED CONTROL.
A test whose program another test takes (PROGRAM-OF) is left alone.
Without --write nothing is written. One line a file: lines before > behind, and what was done or why not.
"""
import argparse, concurrent.futures, os, re, subprocess, sys, tempfile

PREFIXES = "Lgmqefd"
ID = re.compile(r"(?<![A-Za-z0-9_#$.@])([%s])(\d+)\b" % PREFIXES)
QUOTED = re.compile(r'("(?:[^"\\]|\\.)*")')
DIRECTIVE = re.compile(r"^# *([A-Z][A-Z0-9-]*)(\(([A-Z0-9-]+)\))?:( |$)")
SHELL_RUN = re.compile(r"^# *RUN(\([A-Z0-9-]+\))?: run-in-shell\b")
UNIT = re.compile(r"\bf(\d+)(?::[A-Za-z]+)?\b")


def outside_quotes(line, change):
    parts = QUOTED.split(line)
    return "".join(part if index % 2 else change(part) for index, part in enumerate(parts))


def ids_named(line):
    found = []
    outside_quotes(line, lambda part: found.extend(m.group(0) for m in ID.finditer(part)) or part)
    return found


class Record:
    __slots__ = ("kind", "id", "lines", "module", "stub")

    def __init__(self, lines):
        self.lines = lines
        head = lines[0]
        self.kind = head.split(" ", 1)[0]
        m = re.match(r"^\w+ ([%s]\d+)(?: |$)" % PREFIXES, head)
        self.id = m.group(1) if m else None
        m = re.search(r" module=(m\d+)\b", QUOTED.sub('""', head))
        self.module = self.id if self.kind == "module" else (m.group(1) if m else None)
        self.stub = False

    def has_body(self):
        return len(self.lines) > 1

    def text(self):
        if self.stub:
            return [self.lines[0], self.lines[1], "    Unreachable", "}"]
        return self.lines


class Test:
    def __init__(self, text):
        lines = text.split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        self.items = []  # a str: a comment or an empty line. A Record.
        index = 0
        while index < len(lines):
            line = lines[index]
            if not line or line.startswith("#"):
                self.items.append(line)
                index += 1
                continue
            body = [line]
            if line.startswith("function ") and line.endswith("{"):
                while lines[index] != "}":
                    index += 1
                    body.append(lines[index])
            self.items.append(Record(body))
            index += 1
        self.records = [item for item in self.items if isinstance(item, Record)]
        self.by_id = {record.id: record for record in self.records if record.id}

    def own_modules(self):
        return {r.id for r in self.records if r.kind == "module" and not re.search(r" kind=Runtime| flags=\[[^\]]*\bIsLibrary\b", QUOTED.sub('""', r.lines[0]))}

    def directives(self):
        """(index of the item, word, label, rest) of each directive outside a source block."""
        found, in_source = [], False
        for index, item in enumerate(self.items):
            if not isinstance(item, str):
                continue
            if item.startswith("# -- source:"):
                in_source = True
            elif item.startswith("# -- end of source"):
                in_source = False
            elif not in_source:
                m = DIRECTIVE.match(item)
                if m:
                    found.append((index, m.group(1), m.group(3), item[m.end():]))
        return found

    def named_in_comments(self):
        """The ids that a directive or a legend (`# f18 = onWarning`) names: what the test is about."""
        named, in_source = set(), False
        for item in self.items:
            if not isinstance(item, str):
                continue
            if item.startswith("# -- source:"):
                in_source = True
            elif item.startswith("# -- end of source"):
                in_source = False
            elif not in_source and not SHELL_RUN.match(item):
                named.update(name for name in ids_named(item) if name in self.by_id)
        return named

    def functions_named_by_runs(self):
        named = []
        for _, word, _, rest in self.directives():
            if word == "RUN" and not rest.startswith("run-in-shell"):
                named += ["f" + m.group(1) for m in UNIT.finditer(rest)]
        return list(dict.fromkeys(named))


def closure(test, own, whole, roots):
    """The records that the own ones, `entry` and the roots reach by name. A library function outside `whole` counts as its head alone."""
    kept, queue = set(), []

    def take(record):
        if id(record) not in kept:
            kept.add(id(record))
            queue.append(record)

    about = test.named_in_comments()
    rows = {re.search(r" id=(\w+)", r.lines[0]).group(1): ids_named(r.lines[0])[0] for r in test.records if r.kind == "runtimefunction" and ids_named(r.lines[0])}
    test.enclosing = set()  # a function inside a function that the host may call: the reader wants the row of the one that encloses it
    for record in test.records:
        if record.kind == "entry" or record.module in own or record.id in roots or record.id in about:
            take(record)
    while queue:
        record = queue.pop()
        record.stub = record.kind == "function" and record.has_body() and record.module not in own and record.id not in whole
        around = re.search(r" enclosingRuntimeFunction=(\w+)", record.lines[0]) if record.kind == "function" else None
        if around and around.group(1) in rows:
            test.enclosing.add(rows[around.group(1)])
            take(test.by_id[rows[around.group(1)]])
        for line in record.text():
            for name in ids_named(line):
                named = test.by_id.get(name)
                if named is not None:
                    take(named)
    return kept


def write_out(test, kept, roots):
    """The text of the cut test and the new id of each old one."""
    counts, new_ids = {}, {}
    for record in test.records:
        if record.id and id(record) in kept:
            letter = record.id[0]
            new_ids[record.id] = "%s%d" % (letter, counts.get(letter, 0))
            counts[letter] = counts.get(letter, 0) + 1
    renumber = lambda line: outside_quotes(line, lambda part: ID.sub(lambda m: new_ids.get(m.group(0), m.group(0)), part))
    about = test.named_in_comments()
    changed = {old for old, new in new_ids.items() if old != new}
    out, in_source = [], False
    for item in test.items:
        if isinstance(item, str):
            if item.startswith("# -- source:"):
                in_source = True
            elif item.startswith("# -- end of source"):
                in_source = False
            elif not in_source and item.startswith("#") and not re.match(r"^# *RUN(\([A-Z0-9-]+\))?: run-in-shell", item) and changed:
                item = renumber(item)
            out.append(item)
            continue
        if item.id is None and item.kind != "entry":
            named = [name for line in item.lines for name in ids_named(line)]
            if item.kind == "runtimefunction":
                if not all(name in roots or name in about or name in test.enclosing for name in named):
                    continue
            elif not all(name in new_ids for name in named if name in test.by_id):
                continue
        elif item.id is not None and id(item) not in kept:
            continue
        out += [renumber(line) for line in item.text()]
    # Not two empty lines in a row, and none at the end.
    tidy = []
    for line in out:
        if line or (tidy and tidy[-1]):
            tidy.append(line)
    while tidy and not tidy[-1]:
        tidy.pop()
    return "\n".join(tidy) + "\n", new_ids


def with_answers_shown(text):
    """A test of aot/tests/answers says how many answers are those of the shell's own run of the text, not what they are. And a built-in that is written in JavaScript
    and has no `runtimefunction` line gets a stand-in that returns undefined (withStandInLibrary()), in the compiled program and in the shell's own run alike: the count stays.
    run-in-shell fails a run in which the stand-in is called. What else may leave the count as it is shows here: each such RUN prints its answers, in the control and in the cut."""
    shown = 'print("answers: " + got.join(" | ")); print(same + " of "'
    return "\n".join(line.replace('print(same + " of "', shown) if SHELL_RUN.match(line) else line for line in text.split("\n"))


# The ways in which the tests are run at every state of the tree. With every mechanism off the closed ways call library functions that the typed forms never reach.
MODES = ([], ["--mechanisms=none"], ["--option=check=Validate"])


class Oracle:
    def __init__(self, testaot, folder):
        self.testaot, self.folder = testaot, folder
        self.modes = list(range(len(MODES)))  # those in which the test passes as it stands

    def run_in_mode(self, path, mode):
        done = subprocess.run([self.testaot, "run", path, "--show-output", *MODES[mode]], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900)
        said = done.stdout.decode("utf8", "replace").split("\n")
        prints, current = {}, None
        mark = re.compile(r"^OUTPUT " + re.escape(path) + r":(\d+):$")
        verdict = ""
        for line in said:
            m = mark.match(line)
            if m:
                current = prints.setdefault((mode, int(m.group(1))), [])
            elif re.match(r"^\d+ passed, \d+ failed", line):
                verdict, current = line, None
            elif re.match(r"^(FAIL|PASS|TODO|SKIP) ", line):
                current = None
            elif current is not None:
                current.append(line.replace(path, "<file>"))
        return bool(re.match(r"^1 passed, 0 failed", verdict)), prints, verdict

    def run(self, name, text, modes=None, to_the_end=False):
        """(passes in every mode, {(mode, line of the RUN): what it printed}, the verdict of the first mode that fails)"""
        path = os.path.join(self.folder, name)
        with open(path, "w") as file:
            file.write(with_answers_shown(text))
        all_prints, failed = {}, ""
        for mode in self.modes if modes is None else modes:
            passes, prints, verdict = self.run_in_mode(path, mode)
            all_prints.update(prints)
            if not passes and not failed:
                failed = "%s %s" % (" ".join(MODES[mode]) or "as it is run", verdict)
                if not to_the_end:
                    break
        return not failed, all_prints, failed

    def ask(self, name, text, command):
        """What `command` prints for the records of `text`."""
        records = "\n".join(line for line in text.split("\n") if not line.startswith("#"))
        _, prints, _ = self.run(name, "# RUN: %s\n# CHECK: no line says this\n%s" % (command, records), [0])
        return prints.get((0, 1), [])


def cut(test, oracle, name, roots=frozenset(), asks_link=True, bodies=None):
    """`roots`: the functions of the `runtimefunction` records that stay. Without `asks_link` whatever is named keeps its body: for the search.
    `bodies`: the library functions that may keep their bodies; None: all."""
    own = test.own_modules()
    if not own:
        return None, None, "no module of its own"
    library = {r.id for r in test.records if r.kind == "function" and r.has_body() and r.module not in own}
    whole = set(library) if bodies is None else set(bodies)
    rounds = 0
    while True:
        rounds += 1
        kept = closure(test, own, whole, roots)
        text, new_ids = write_out(test, kept, roots)
        if not asks_link:
            return text, new_ids, {f for f in whole if id(test.by_id[f]) in kept}
        old_ids = {new: old for old, new in new_ids.items()}
        said = oracle.ask(name + ".link.ir", text, "link functions")
        listed = [m for m in (re.match(r"^function (f\d+)( unreached\b)?", line) for line in said) if m]
        if not listed:
            # The build stops early, which is what some tests are about: Link says nothing of any function.
            reached = set()
        else:
            reached = {old_ids[m.group(1)] for m in listed if not m.group(2) and m.group(1) in old_ids}
        now = {f for f in whole if f in reached and id(test.by_id[f]) in kept}
        if now == whole:
            return text, new_ids, whole
        whole = now


def mapped(lines, new_ids, by_id, thins=False):
    """The control's print with the ids of the cut. None in place of a line that names a record that is gone; with `thins` the line without those names:
    `referrers f0, f1, f120` is `referrers f0, f1` where f120 is gone."""
    out = []
    for line in lines:
        gone = False

        def change(m):
            nonlocal gone
            name = m.group(0)
            if name in new_ids:
                return new_ids[name]
            if name in by_id:
                gone = True
                return "\0"
            return name

        text = outside_quotes(line, lambda part: ID.sub(change, part))
        if gone and thins:
            text = re.sub(r", \0|\0, |\0", "", text)
            gone = False
        out.append(None if gone else text)
    return out


# --walks-may-differ: what Link and the plan say of a function may hang on the whole program (a guard of parameters stands only while something in the program calls an unknown callee with arguments).
# Then the check lines, the answers and the red control are all that holds the cut.
WALKS_MAY_DIFFER = False


def same_print(control, behind, new_ids, by_id, names_functions):
    """Empty: the same. Else the first difference. `names_functions`: the RUN says which functions it prints."""
    want = mapped(control, new_ids, by_id)
    if None not in want and want == behind:
        return ""
    if names_functions and WALKS_MAY_DIFFER:
        return ""
    if names_functions:
        want = [line if line is not None else "<a line that names what is gone>" for line in want]
        for index, (a, b) in enumerate(zip(want, behind)):
            if a != b:
                return "line %d: %s | %s" % (index + 1, a[:150], b[:150])
        return "%d lines | %d lines" % (len(want), len(behind))
    # A print of the whole program (`link lookups`, `bytes-by-form`): what the cut prints is what the control printed of what is kept. Such a print goes by the numbers of keys and
    # strings, which are given where each is first named, and counts over the whole program: so each line of the cut is one of the control's, as often, whatever the order, numbers aside.
    import collections
    masked = lambda line: re.sub(r"\d+", "0", line)
    have = collections.Counter(masked(line) for line in mapped(control, new_ids, by_id, True))
    for index, line in enumerate(behind):
        if not have[masked(line)]:
            if WALKS_MAY_DIFFER:
                continue
            return "line %d is none of the control's, numbers aside: %s" % (index + 1, line[:200])
        have[masked(line)] -= 1
    return ""


def emptied(text, functions):
    test = Test(text)
    some = False
    for function in functions:
        record = test.by_id.get(function)
        if record is not None and record.has_body():
            record.stub = some = True
    if not some:
        return None
    return "\n".join(line for item in test.items for line in ([item] if isinstance(item, str) else item.text())) + "\n"


def smallest(items, holds):
    """ddmin: a part of `items` for which `holds` is true and that loses it with any one member taken out (as far as the search sees)."""
    pieces = 2
    while len(items) >= 2:
        size = (len(items) + pieces - 1) // pieces
        chunks = [items[index:index + size] for index in range(0, len(items), size)]
        for chunk in chunks:
            if len(chunk) < len(items) and holds(chunk):
                items, pieces = chunk, 2
                break
        else:
            for index in range(len(chunks)):
                rest = [item for number, chunk in enumerate(chunks) if number != index for item in chunk]
                if rest and len(chunks) > 2 and holds(rest):
                    items, pieces = rest, max(pieces - 1, 2)
                    break
            else:
                if pieces >= len(items):
                    break
                pieces = min(len(items), pieces * 2)
    return items


def work(path, arguments):
    try:
        return work_on(path, arguments)
    except Exception as error:  # one file's trouble is one line
        return path, 0, 0, "LEFT: %s: %s" % (type(error).__name__, str(error)[:200])


def work_on(path, arguments):
    text = open(path).read()
    before = text.count("\n")
    with tempfile.TemporaryDirectory(prefix="cut-ir-test-") as folder:
        oracle = Oracle(arguments.testaot, folder)
        name = os.path.basename(path)
        test = Test(text)
        if not test.own_modules():
            return path, before, before, "LEFT: no module of its own"
        takers = arguments.takers.get(os.path.realpath(path))
        if takers:
            return path, before, before, "LEFT: %d other tests take its program (PROGRAM-OF), and their prints are not held here: %s" % (len(takers), " ".join(takers[:4]))
        if not any(r.module not in test.own_modules() for r in test.records if r.module):
            return path, before, before, "LEFT: nothing but the program's own files"
        passes, control, verdict = oracle.run(name, text)
        if not passes:
            return path, before, before, "LEFT: it does not pass as it stands: " + verdict
        names_functions = {}
        for index, word, _, rest in test.directives():
            if word == "RUN":
                for mode in oracle.modes:
                    names_functions[(mode, index + 1)] = not rest.startswith("run-in-shell") and bool(UNIT.search(rest))
        probes = [0]

        kept_whole = [set()]

        def attempt(roots, asks_link=True, bodies=None):
            """(why not, the text, the new ids, what it printed)"""
            probes[0] += 1
            behind_text, new_ids, kept_whole[0] = cut(test, oracle, name, frozenset(roots), asks_link, bodies)
            passes, prints, verdict = oracle.run(name, behind_text, None, True)
            for line in control:
                if line not in prints or same_print(control[line], prints[line], new_ids, test.by_id, names_functions.get(line, False)):
                    focus.add(line)
            if not passes:
                return "the cut test does not pass: " + verdict, behind_text, new_ids, prints
            # The directives stand where they stood: nothing above the records is taken out.
            if sorted(control) != sorted(prints):
                return "other RUN lines printed: %s | %s" % (sorted(control), sorted(prints)), behind_text, new_ids, prints
            for line in sorted(control):
                difference = same_print(control[line], prints[line], new_ids, test.by_id, names_functions.get(line, False))
                if difference:
                    return "the RUN of line %d prints otherwise%s: %s" % (line[1], " under " + " ".join(MODES[line[0]]) if line[0] else "", difference), behind_text, new_ids, prints
            return "", behind_text, new_ids, prints

        def holds(roots, bodies=None):
            """For the search: the RUN lines that have differed so far, and nothing else of the test: do they print what they printed?"""
            probes[0] += 1
            behind_text, new_ids, _ = cut(test, oracle, name, frozenset(roots), False, bodies)
            lines = behind_text.split("\n")
            general = [line for line in lines if re.match(r"^# *(OPTIONS|LIBRARY)\b", line)]
            records = [line for line in lines if not line.startswith("#")]
            for mode in sorted({mode for mode, _ in focus}):
                asked = sorted(line for m, line in focus if m == mode)
                head = list(general)
                for number, line in enumerate(asked):
                    head += ["# RUN(P%d): %s" % (number, re.sub(r"^# *RUN(\([A-Z0-9-]+\))?: ", "", lines[line - 1])), "# P%d: no line says this" % number]
                _, prints, _ = oracle.run(name, "\n".join(head + records) + "\n", [mode])
                for number, line in enumerate(asked):
                    printed = prints.get((mode, len(general) + 2 * number + 1))
                    if printed is None or same_print(control[(mode, line)], printed, new_ids, test.by_id, names_functions.get((mode, line), False)):
                        return False
            return True

        focus = set()
        roots = []
        why, behind_text, new_ids, prints = attempt(roots)
        first_why = why
        if why:
            # Which of the functions that the host may call must stay roots? The likeliest first: those that the control's prints name, by id or by the name of
            # the row, then those whose name ends in a name that the program's own records ask for (`ArrayPrototypeAt`, key="at").
            rows = {ids_named(r.lines[0])[0]: re.search(r" id=(\w+)", r.lines[0]).group(1) for r in test.records if r.kind == "runtimefunction" and ids_named(r.lines[0])}
            candidates = list(rows)
            printed = "\n".join(line for lines in control.values() for line in lines)
            said = set(ids_named(QUOTED.sub('""', printed)))
            words = set(re.findall(r"\w+", printed))
            own = test.own_modules()
            asked = {key.lower() for r in test.records if r.module in own for line in r.lines for key in re.findall(r'key="(\w+)"', line)}
            named = [f for f in candidates if f in said or rows[f] in words]
            likely = named + [f for f in candidates if f not in named and any(rows[f].lower().endswith(key) for key in asked)]
            roots = None
            for some in (named, likely, candidates):
                if some and not attempt(some, False)[0]:
                    # The search asks the RUN lines that have differed. What it finds is held against the whole test; a RUN line that differs then joins them, and it looks again.
                    for _ in range(4):
                        roots = smallest(some, holds)
                        seen = len(focus)
                        why, behind_text, new_ids, prints = attempt(roots)
                        if not why or len(focus) == seen:
                            break
                    if why:
                        roots = smallest(some, lambda part: not attempt(part, False)[0])
                        why, behind_text, new_ids, prints = attempt(roots)
                    break
                if arguments.quick and some is likely:
                    break
        if roots is not None and not why and behind_text.count("\n") > arguments.fat:
            # Still fat: a print hangs on something deep in what is reached. Which of the bodies that are left does it hang on?
            all_bodies = sorted(kept_whole[0], key=lambda f: int(f[1:]))
            if not focus:
                focus.update(control)
            for _ in range(4):
                bodies = smallest(all_bodies, lambda part: holds(roots, part))
                if len(bodies) == 1 and holds(roots, []):
                    bodies = []
                seen = len(focus)
                thinner = attempt(roots, False, bodies)
                if not thinner[0]:
                    why, behind_text, new_ids, prints = thinner
                    break
                if len(focus) == seen:
                    break
        behind = behind_text.count("\n")
        if arguments.keep:
            os.makedirs(arguments.keep, exist_ok=True)
            with open(os.path.join(arguments.keep, name), "w") as file:
                file.write(behind_text)
        if arguments.diff and why:
            import difflib
            for line in sorted(control):
                want = [text if text is not None else "<a line that names what is gone>" for text in mapped(control[line], new_ids, test.by_id)]
                for text in list(difflib.unified_diff(want, prints.get(line, []), "control:%s" % (line,), "cut:%s" % (line,), lineterm="", n=1))[:arguments.diff]:
                    print("    " + text[:260])
        if why:
            return path, before, behind, "NOT CUT: %s%s" % (why, "" if roots is not None else " (and with every function that the host may call kept as a root it is no better)")
        if behind >= before:
            return path, before, before, "LEFT: nothing to cut"
        host = ""
        if roots:
            names = {ids_named(r.lines[0])[0]: re.search(r" id=(\w+)", r.lines[0]).group(1) for r in test.records if r.kind == "runtimefunction" and ids_named(r.lines[0])}
            host = ", the host's %s stay (without: %s)" % (" ".join(names.get(f, f) for f in roots), first_why[:90])

        # THE RED CONTROL. A function whose checks are all of the kind "no line says .." cannot turn them red: there the uncut test is asked too.
        cut_test = Test(behind_text)
        named = cut_test.functions_named_by_runs()
        if not named:
            entry = next((r for r in cut_test.records if r.kind == "entry"), None)
            module = cut_test.by_id.get(re.search(r" module=(m\d+)", entry.lines[0]).group(1)) if entry else None
            body = re.search(r" body=(f\d+)", module.lines[0]) if module else None
            named = [body.group(1)] if body else []
        if not named:
            return path, before, behind, "NOT CUT: no function to take out for the red control"
        old_ids = {new: old for old, new in new_ids.items()}
        red, green = [], []
        for function in named:
            without = emptied(behind_text, [function])
            if without is None:
                continue
            (green if oracle.run(name, without)[0] else red).append(function)
        weak = ""
        if green:
            own_fault = [f for f in green if not oracle.run(name, emptied(text, [old_ids.get(f, f)]))[0]]
            if own_fault:
                return path, before, behind, "NOT CUT: THE RED CONTROL STAYS GREEN without the body of %s, AND THE UNCUT TEST TURNS RED THERE" % " ".join(own_fault)
            weak = ", green without %s as the uncut test is" % " ".join(green)
        if not red:
            # `CHECK-LABEL: code f1:Body` and no more: the test asks that the build comes to its end. `CHECK-NOT` alone: that a line is not there. No body taken out turns those red, cut or uncut.
            weak = ", NO RED CONTROL: ITS CHECKS ASK FOR NO LINE OF A BODY: green without %s AS THE UNCUT TEST IS" % " ".join(green)
        if arguments.write:
            with open(path, "w") as file:
                file.write(behind_text)
        moved = sum(1 for old, new in new_ids.items() if old != new)
        return path, before, behind, "%s: %d RUN lines print the same in %d modes" % ("CUT" if arguments.write else "CAN BE CUT", len({line for _, line in control}), len(oracle.modes)) + (" (THE WALKS MAY DIFFER)" if WALKS_MAY_DIFFER else "") + ", red without %s%s%s, %d ids numbered anew, %d probes" % (" ".join(red) or "nothing", weak, host, moved, probes[0])
        return path, before, behind, "%s: %d RUN lines print the same in %d modes, red without %s%s%s, %d ids numbered anew, %d probes" % (
            "CUT" if arguments.write else "CAN BE CUT", len({line for _, line in control}), len(oracle.modes), " ".join(red) or "nothing", weak, host, moved, probes[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--testaot", required=True)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--bar", type=int, default=0, help="only files with more lines than this")
    parser.add_argument("--diff", type=int, default=0, help="print so many lines of what each RUN prints otherwise")
    parser.add_argument("--keep", help="a folder for the cut texts, whatever becomes of them")
    parser.add_argument("--quick", action="store_true", help="the search does not go on to every function that the host may call")
    parser.add_argument("--fat", type=int, default=2000, help="a cut with more lines than this is searched for the bodies that it can do without")
    parser.add_argument("paths", nargs="+")
    parser.add_argument("--walks-may-differ", action="store_true", help="a RUN that names functions need not print what it printed: its check lines hold it")
    arguments = parser.parse_args()
    global WALKS_MAY_DIFFER
    WALKS_MAY_DIFFER = arguments.walks_may_differ
    files = []
    for path in arguments.paths:
        if os.path.isdir(path):
            files += sorted(os.path.join(folder, name) for folder, _, names in os.walk(path) for name in names if name.endswith(".ir"))
        else:
            files.append(path)
    files = [path for path in files if open(path, errors="replace").read().count("\n") > arguments.bar]
    # `# PROGRAM-OF: ../link/other.ir`: a test without records of its own runs on those of another.
    arguments.takers = {}
    roots = {next((os.sep.join(parts[:index + 1]) for index in range(len(parts) - 1, 0, -1) if parts[index] == "tests"), os.path.dirname(os.path.realpath(path)))
             for path in files for parts in [os.path.realpath(path).split(os.sep)]}
    for root in roots:
        for folder, _, names in os.walk(root):
            for name in names:
                if name.endswith(".ir"):
                    for line in open(os.path.join(folder, name), errors="replace"):
                        if not line.startswith("#"):
                            break
                        found = re.match(r"^# *PROGRAM-OF: *(\S+)", line)
                        if found:
                            arguments.takers.setdefault(os.path.realpath(os.path.join(folder, found.group(1))), []).append(os.path.relpath(os.path.join(folder, name), root))
    out = total_before = total_behind = 0
    with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
        for path, before, behind, said in (done.result() for done in concurrent.futures.as_completed([pool.submit(work, path, arguments) for path in files])):
            done = said.startswith(("CUT", "CAN BE CUT"))
            total_before += before
            total_behind += behind if done else before
            out += done
            print("%6d > %6d  %s  %s" % (before, behind, path, said), flush=True)
    print("%d of %d files, %d > %d lines" % (out, len(files), total_before, total_behind))


if __name__ == "__main__":
    main()
