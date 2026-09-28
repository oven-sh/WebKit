# compare.py <audit> <reference> <output> <how many differences to show>

import sys

audit, reference, output, show = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])


def report(counts, differences):
    print(audit + ": " + ", ".join("%d %s" % (count, what) for what, count in counts))
    for difference in differences[:show]:
        print("    " + difference)


if audit == "own-attributes":
    def load(path):
        with open(path) as file:
            return {line.split(" ", 1)[0]: dict(item.rsplit(":", 1) for item in line.split()[1:] if ":" in item) for line in file}

    expected, actual = load(reference), load(output)
    missing = extra = kinds = same = 0
    differences = []
    for name in sorted(expected):
        if name == "builtins":
            continue
        if name not in actual:
            differences.append("no class " + name)
            continue
        absent = sorted(set(expected[name]) - set(actual[name]))
        added = sorted(set(actual[name]) - set(expected[name]))
        other = sorted(a for a in expected[name] if a in actual[name] and expected[name][a] != actual[name][a])
        missing += len(absent)
        extra += len(added)
        kinds += len(other)
        same += len(expected[name]) - len(absent) - len(other)
        if absent or added or other:
            differences.append("%s: lacks %s; has besides %s; of another kind %s" % (name, " ".join(absent) or "-", " ".join(added) or "-", " ".join(other) or "-"))
    report([("the same", same), ("missing", missing), ("that CPython does not have", extra), ("of another kind", kinds)], differences)
elif audit == "language-features":
    def load(path):
        with open(path) as file:
            return dict(line.rstrip("\n").split(" => ", 1) for line in file if " => " in line)

    expected, actual = load(reference), load(output)
    groups = {}
    for key in expected:
        group = groups.setdefault(key.split(" | ")[0], [0, 0])
        group[0] += actual.get(key) == expected[key]
        group[1] += 1
    wrong = [key for key in expected if actual.get(key) != expected[key]]
    report([("of %d the same" % len(expected), len(expected) - len(wrong))] + [("of %d in %s" % (total, name), same) for name, (same, total) in groups.items()],
           ["%s: %s | here: %s" % (key, expected[key][:100], actual.get(key, "nothing")[:100]) for key in wrong])
elif audit == "syntax-errors":
    def load(path):
        with open(path) as file:
            return dict(line.rstrip("\n").split(" | ", 1) for line in file if " | " in line)

    expected, actual = load(reference), load(output)
    groups = {}
    for key in expected:
        group = groups.setdefault(key.split(" ")[0], [0, 0])
        group[0] += actual.get(key) == expected[key]
        group[1] += 1
    wrong = [key for key in expected if actual.get(key) != expected[key]]
    report([("of %d the same" % len(expected), len(expected) - len(wrong))] + [("of %d from %s" % (total, name), same) for name, (same, total) in groups.items()],
           ["%s: %s\n        here: %s" % (key, expected[key][:170], actual.get(key, "nothing")[:170]) for key in wrong])
elif audit in ("operations", "methods", "special-methods"):
    def load(path):
        with open(path) as file:
            return dict(line.rstrip("\n").split(" | ", 1) for line in file if " | " in line)

    expected, actual = load(reference), load(output)
    wrong = [key for key in expected if actual.get(key) != expected[key]]
    report([("of %d lines the same" % len(expected), len(expected) - len(wrong)), ("things tried", sum(int(value.split()[0]) for value in expected.values()))], wrong)
elif audit == "syntax-trees":
    def load(path):
        with open(path) as file:
            return dict(line.rstrip("\n").split(" | ", 1) for line in file if " | " in line)

    expected, actual = load(reference), load(output)
    wrong = [key for key in expected if actual.get(key) != expected[key]]
    report([("of %d fields the same" % len(expected), len(expected) - len(wrong)), ("things tried", sum(int(value.split()[0]) for value in expected.values()))], wrong)
else:
    def load(path):
        with open(path) as file:
            return dict(line.rstrip("\n").split(" | ", 1) for line in file if " | " in line)

    expected, actual = load(reference), load(output)
    wrong = [key for key in expected if key in actual and actual[key] != expected[key]]
    absent = [key for key in expected if key not in actual]
    added = [key for key in actual if key not in expected]
    report([("the same", len(expected) - len(wrong) - len(absent)), ("different", len(wrong)), ("of methods that are missing", len(absent)), ("of methods that CPython does not have", len(added))],
           ["%s: %s | here: %s" % (key, expected[key], actual[key]) for key in wrong])
