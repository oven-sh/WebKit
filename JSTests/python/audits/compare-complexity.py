# compare-complexity.py <what CPython printed> <what this engine printed>
#
# What complexity.py found to go up faster with n here than in CPython, and what takes many times as long.
import sys


def load(path):
    found = {}
    for line in open(path, errors="replace"):
        # From the right, since what is being timed may have one in it.
        parts = [p.strip() for p in line.rsplit(" | ", 3)]
        if len(parts) == 4:
            found[parts[0]] = parts[1:]
    return found


theirs, ours = load(sys.argv[1]), load(sys.argv[2])
steeper, slower, failed = [], [], []
for label, (n, power, each) in ours.items():
    if label not in theirs:
        continue
    their_power, their_each = theirs[label][1], float(theirs[label][2])
    try:
        power_value = float(power)
    except ValueError:
        (steeper if power == "too slow" else failed).append((9, label, power, their_power, each, theirs[label][2]))
        continue
    try:
        their_value = float(their_power)
    except ValueError:
        continue
    if power_value - their_value > 0.35 and power_value > 1.35:
        steeper.append((power_value - their_value, label, power, their_power, each, theirs[label][2]))
    elif their_each and float(each) / their_each > 8:
        slower.append((float(each) / their_each, label, power, their_power, each, theirs[label][2]))
for title, rows in (("goes up faster", steeper), ("many times as long", slower), ("did not finish", failed)):
    print("----", title, len(rows))
    for row in sorted(rows, reverse=True):
        print("%-46s power %-9s CPython %-6s ns each %8s  CPython %8s" % row[1:])
print(len(ours), "in all")
