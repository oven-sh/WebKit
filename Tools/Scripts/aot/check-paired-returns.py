#!/usr/bin/env python3
"""Checks that the stubs of an ARM64 image return the way the processor predicts.

  check-paired-returns.py --image <image or executable> --map <map>

The processor predicts the target of a RET from a stack of the addresses that BL and BLR left behind. A stub that computes a return
address into x30 and jumps to the callee, or that returns with BR, leaves that stack off by one. Then its own return is mispredicted,
and so is every return to a frame that was already live, up to the depth of that stack.

Reads the machine code of every copy of the stubs, and fails on a stub that
  - computes x30 with ADR or ADRP, unless the next instruction jumps to a stub that never returns (a TN row of the map), or
  - copies x30 to another register and then leaves with BR through that register.
So that it cannot pass with nothing to check, it also fails unless at least one stub calls with BLR.

The image is only read, so an executable that holds one can be
given as it is, with the map that was written when it was built (--aotMapFilePath).
"""
import argparse, bisect, os, struct, sys

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--image', type=os.path.abspath, required=True)
parser.add_argument('--map', type=os.path.abspath, required=True)
args = parser.parse_args()

MAGIC = b'BUNAOT01'


def find_image(data):
    start = data.find(MAGIC)
    while 0 <= start <= len(data) - 56:
        image_size, code_offset, code_size = struct.unpack_from('<QQQ', data, start + 32)
        if code_size and code_offset + code_size <= image_size <= len(data) - start:
            return start, code_offset, code_size
        start = data.find(MAGIC, start + 1)
    sys.exit('%s holds no image' % args.image)


data = open(args.image, 'rb').read()
start, code_offset, code_size = find_image(data)
names, copies, functions, noreturn_stubs = [], [], [], set()
for line in open(args.map, errors='replace'):
    fields = line.rstrip('\n').split('\t')
    if fields[0] == 'T':
        names.append((int(fields[1]), fields[2]))
    elif fields[0] == 'TN':
        noreturn_stubs.add(int(fields[1]))
    elif fields[0] == 'C':
        copies.append(int(fields[1]))
    elif fields[0] == 'F':
        functions.append(int(fields[2]))
if not names or not copies:
    sys.exit('%s names no stubs' % args.map)
shortest = {}
for offset, name in names:
    if offset not in shortest or len(name) < len(shortest[offset]):
        shortest[offset] = name
names = sorted(shortest.items())
offsets = [offset for offset, _ in names]
size = min([at for at in functions if at > copies[0]] + [code_size]) - copies[0]

RET = 0xd65f03c0
is_adr_to_link_register = lambda word: word & 0x1f00001f == 0x1000001e
is_b = lambda word: word & 0xfc000000 == 0x14000000
is_br = lambda word: word & 0xfffffc1f == 0xd61f0000
is_blr = lambda word: word & 0xfffffc1f == 0xd63f0000
is_copy_of_link_register = lambda word: word & 0xffffffe0 == 0xaa1e03e0
displacement_of_b = lambda word: 4 * (((word & 0x3ffffff) ^ 0x2000000) - 0x2000000)
register_of_branch = lambda word: word >> 5 & 31


def stub_at(offset):
    index = bisect.bisect_right(offsets, offset) - 1
    return names[index][1], offset - names[index][0]


def jumps_to_noreturn_stub(words, index):
    return index < len(words) and is_b(words[index]) and 4 * index + displacement_of_b(words[index]) in noreturn_stubs


problems = []
for copy in copies:
    words = struct.unpack_from('<%dI' % (size // 4), data, start + code_offset + copy)
    if RET not in words:
        print('not ARM64 code: nothing to check')
        sys.exit(0)
    for index, word in enumerate(words):
        if is_adr_to_link_register(word) and not jumps_to_noreturn_stub(words, index + 1):
            problems.append('%s+%d computes x30' % stub_at(4 * index))
        elif is_br(word) and any(is_copy_of_link_register(earlier) and earlier & 31 == register_of_branch(word) for earlier in words[max(0, index - 3):index]):
            problems.append('%s+%d returns with br x%d' % (*stub_at(4 * index), register_of_branch(word)))
    if not any(is_blr(word) for word in words):
        problems.append('no stub calls with blr')

seen = set()
for problem in problems:
    if problem not in seen:
        seen.add(problem)
        print(problem)
print('%d stubs in %d bytes, %d %s: %d unpaired returns' % (len(names), size, len(copies), 'copy' if len(copies) == 1 else 'copies', len(seen)))
sys.exit(1 if seen else 0)
