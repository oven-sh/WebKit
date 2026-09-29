#!/usr/bin/env python3
# Prints the bucket table of results/dr2.txt: execution = main - compile - gc.
import sys
rows = []
for line in open('/workspace/wkbuild/results/dr2.txt'):
    if 'threadcount:' not in line or 'compile main=' not in line:
        continue
    head, rest = line.split(' | ', 1)
    w, label = head.split(': ', 1)
    def field(after, key):
        i = rest.index(after)
        j = rest.index(key, i) + len(key)
        k = j
        while k < len(rest) and rest[k].isdigit():
            k += 1
        return int(rest[j:k])
    main = field('threadcount:', 'main=')
    comp = field('threadcount-wrap: compile', 'main=')
    cc = field('threadcount-wrap: compile', 'calls=')
    gc = field('threadcount-wrap: gc', 'main=')
    rows.append((w, label, main, comp, cc, gc, main - comp - gc))
base = {}
for w, label, main, comp, cc, gc, ex in rows:
    if label in ('5000, four off', 'scale 10, four off'):
        base[(w, label.split(',')[0])] = (main, comp, cc, gc, ex)
pct = lambda a, c: (a - c) / c * 100
for w, label, main, comp, cc, gc, ex in rows:
    key = label.split(',')[0]
    if key == '500':
        key = '5000'
    b = base.get((w, key))
    if not b:
        print(w, label, main, comp, cc, gc, ex)
        continue
    print("%-16s %-36s exec %6.3f G (%+5.1f%%)  compile %6.3f G (%+5.1f%%, %d calls %+d)  gc %5.3f G  total %7.3f G (%+5.2f%%)" % (
        w, label, ex / 1e9, pct(ex, b[4]), comp / 1e9, pct(comp, b[1]), cc, cc - b[2], gc / 1e9, main / 1e9, pct(main, b[0])))
