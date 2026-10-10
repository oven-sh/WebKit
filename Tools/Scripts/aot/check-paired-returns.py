#!/usr/bin/env python3
"""Checks that the stubs of an ARM64 image return the way the processor predicts.

  check-paired-returns.py <jsc>
  check-paired-returns.py --image <image or executable> --map <map>

The processor predicts the target of a RET from a stack of the addresses that BL and BLR left behind. A stub that computes a return
address into x30 and jumps to the callee, or that returns with BR, leaves that stack off by one. Then its own return is mispredicted,
and so is every return to a frame that was already live, up to the depth of that stack.

Reads the machine code of every copy of the stubs, and fails on a stub that
  - computes x30 with ADR or ADRP, or
  - copies x30 to another register and then leaves with BR through that register.
So that it cannot pass for want of stubs to look at, it also fails unless the stub that calls return to after a call with an
argument list starts with a BLR, and the stubs for cold operations of functions without a frame return with RET through a register.

With <jsc> it compiles an empty program and looks at that image. The image is only read, so an executable that holds one can be
given as it is, with the map that was written when it was built (--aotMapFilePath).
"""
import argparse, bisect, os, struct, subprocess, sys, tempfile

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('jsc', nargs='?', type=os.path.abspath)
parser.add_argument('--image', type=os.path.abspath)
parser.add_argument('--map', type=os.path.abspath)
args = parser.parse_args()
if bool(args.jsc) == bool(args.image) or bool(args.image) != bool(args.map):
    parser.error('give either <jsc>, or both --image and --map')

if args.jsc:
    directory = tempfile.TemporaryDirectory()
    args.image, args.map, program = (os.path.join(directory.name, name) for name in ('image', 'map', 'empty.js'))
    open(program, 'w').close()
    subprocess.run([args.jsc, '--useJIT=false', '--useDollarVM=true', '--writeAOTImageTo=' + args.image, '--aotMapFilePath=' + args.map, program], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    if not os.path.exists(args.image) or not os.path.exists(args.map):
        sys.exit('%s wrote no image' % args.jsc)

data = open(args.image, 'rb').read()
start = data.find(b'BUNAOT01')
if start < 0:
    sys.exit('%s holds no image' % args.image)
code_offset, code_size = struct.unpack_from('<QQ', data, start + 24)
names, copies, functions = [], [], []
for line in open(args.map, errors='replace'):
    fields = line.rstrip('\n').split('\t')
    if fields[0] == 'T':
        names.append((int(fields[1]), fields[2]))
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
is_blr = lambda word: word & 0xfffffc1f == 0xd63f0000
is_br = lambda word: word & 0xfffffc1f == 0xd61f0000
is_ret_through_other_register = lambda word: word & 0xfffffc1f == 0xd65f0000 and word != RET
is_copy_of_link_register = lambda word: word & 0xffffffe0 == 0xaa1e03e0
register_of_branch = lambda word: word >> 5 & 31


def stub_at(offset):
    index = bisect.bisect_right(offsets, offset) - 1
    return names[index][1], offset - names[index][0]


def words_of(words, name):
    index = next(i for i, (_, other) in enumerate(names) if other == name)
    end = offsets[index + 1] if index + 1 < len(offsets) else size
    return words[offsets[index] // 4:end // 4]


problems = []
for copy in copies:
    words = struct.unpack_from('<%dI' % (size // 4), data, start + code_offset + copy)
    if RET not in words:
        print('not ARM64 code: nothing to check')
        sys.exit(0)
    for index, word in enumerate(words):
        if is_adr_to_link_register(word):
            problems.append('%s+%d computes x30' % stub_at(4 * index))
        elif is_br(word) and any(is_copy_of_link_register(earlier) and earlier & 31 == register_of_branch(word) for earlier in words[max(0, index - 3):index]):
            problems.append('%s+%d returns with br x%d' % (*stub_at(4 * index), register_of_branch(word)))
    present = {name for _, name in names}
    for name in ('ReturnFromCallWithList', 'LeafColdOperationVoid', 'LeafColdOperationValue'):
        if name not in present:
            problems.append('there is no stub named %s' % name)
    if 'ReturnFromCallWithList' in present and not is_blr(words_of(words, 'ReturnFromCallWithList')[0]):
        problems.append('ReturnFromCallWithList does not start with the call that it is returned to from')
    for name in ('LeafColdOperationVoid', 'LeafColdOperationValue'):
        if name in present and not any(is_ret_through_other_register(word) for word in words_of(words, name)):
            problems.append('%s does not return with ret through a register' % name)

seen = set()
for problem in problems:
    if problem not in seen:
        seen.add(problem)
        print(problem)
print('%d stubs in %d bytes, %d %s: %d unpaired returns' % (len(names), size, len(copies), 'copy' if len(copies) == 1 else 'copies', len(seen)))
sys.exit(1 if seen else 0)
