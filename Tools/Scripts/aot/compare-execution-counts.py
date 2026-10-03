#!/usr/bin/env python3
"""Checks that two builds of a program do the same work, before their speed is compared.

  compare-execution-counts.py <coverage A> <counts A> <coverage B> <counts B> [--since <counts A> <counts B>] [--tolerance PERCENT]

A miscompiled program need not crash. If what goes wrong is caught by the program's own handlers, it may print the same output and
just do less, and then it looks faster. So compile both builds with --aotTypeCoveragePath=<coverage> --useAOTTypeCoverageCounters=1,
run both on the same input with --aotTypeCoverageCountsPath=<counts>, and compare.

Compared are the operations that optimizations leave in place: how often they ran, and at how many places in the program at least
one ran. Calls, returns and accesses to elements are listed but not compared, because inlining and the loop optimizations remove
them. --since takes copies of the two counts files made earlier in the same runs, for example once the program had started, and
subtracts them.

The exit status is 1 if a count or a number of places differs by more than the tolerance (1 percent unless given) and by more than 3.
Make the two runs one right after the other on a machine that is equally busy: what a program does on timers falls inside a slow
run and outside a fast one. And run the same build twice first, to see how much the program varies by itself.
"""
import argparse, array, collections, sys

COMPARED = ['op_catch', 'op_throw', 'op_new_func', 'op_new_func_exp', 'op_create_lexical_environment', 'op_new_object', 'op_new_array', 'op_get_by_id', 'op_put_by_id']
LISTED = ['op_get_by_val', 'op_put_by_val', 'op_call', 'op_construct', 'op_ret']


def load(path):
    counts = array.array('I')
    data = open(path, 'rb').read()
    counts.frombytes(data[:len(data) // 4 * 4])
    return counts


def executions(coverage, path, since):
    counts, earlier = load(path), load(since) if since else []
    seen, ran, places = set(), collections.Counter(), collections.Counter()
    for line in open(coverage, errors='replace'):
        if line[0] != 'O':
            continue
        fields = line.rstrip('\n').split('\t')
        if len(fields) < 14 or not fields[13]:
            continue
        i = int(fields[13])
        if i in seen or i >= len(counts):
            continue
        seen.add(i)
        n = counts[i] - (earlier[i] if i < len(earlier) else 0)
        if n > 0:
            ran[fields[4]] += n
            places[fields[4]] += 1
    return ran, places


parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('coverageA')
parser.add_argument('countsA')
parser.add_argument('coverageB')
parser.add_argument('countsB')
parser.add_argument('--since', nargs=2, metavar=('COUNTS_A', 'COUNTS_B'), default=(None, None))
parser.add_argument('--tolerance', type=float, default=1)
args = parser.parse_args()
ranA, placesA = executions(args.coverageA, args.countsA, args.since[0])
ranB, placesB = executions(args.coverageB, args.countsB, args.since[1])


def differs(a, b):
    return abs(a - b) > 3 and abs(a - b) * 100 > args.tolerance * max(a, b)


failed = False
print('%-32s %14s %14s %9s %9s' % ('', 'ran in A', 'ran in B', 'places A', 'places B'))
for opcode in COMPARED + LISTED:
    bad = opcode in COMPARED and (differs(ranA[opcode], ranB[opcode]) or differs(placesA[opcode], placesB[opcode]))
    failed |= bad
    print('%-32s %14s %14s %9s %9s%s' % (opcode[3:], format(ranA[opcode], ','), format(ranB[opcode], ','), format(placesA[opcode], ','), format(placesB[opcode], ','), '  DIFFERS' if bad else '' if opcode in COMPARED else '  (not compared)'))
print('%-32s %14s %14s' % ('all operations', format(sum(ranA.values()), ','), format(sum(ranB.values()), ',')))
print('The two builds do not do the same work.' if failed else 'The two builds do the same work.')
sys.exit(1 if failed else 0)
