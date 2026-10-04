#!/usr/bin/env python3
"""Checks that the stubs of an ARM64 image change no register that they are not declared to change.

  check-stub-clobbers.py <jsc>
  check-stub-clobbers.py --image <image or executable> --map <map>
  check-stub-clobbers.py ... --print [--stub <name>]...

Compiled code keeps values in registers across a call of a stub that is declared to preserve them. The declaration is in the map, one
line for each such stub: K, its name, the registers it may change, and the register that a variant of it which takes its operand in
another register may change besides. A stub that changes another register, on however rare a path, corrupts a value of its caller.

Follows every path from the entry of each declared stub, of each variant of it for a register, and of each thunk that names an
operation and jumps to it, through branches and through jumps to other stubs, and keeps for every register whether it still holds
the caller's value. A register that is stored to the stack while it holds the caller's value, and loaded back from that place, holds it again. A
BLR, or a BL that leaves the stubs, is a call by the C convention: it changes x0 to x17, d0 to d7 and d16 to d31. (x18 is the
platform's, x30 is the return address: neither is ever allocated, so neither is looked at.) A BL to another stub changes what that
stub changes.

A path ends
  - in RET: what is changed there has to be declared, and the stack pointer has to be the caller's;
  - in the jump to the entry for exceptions: nothing counts, since a handler reads its state from the frame;
  - in BRK;
  - in any other BR, or a jump that leaves the stubs: a tail jump, after which nothing is known. A declared stub must not have one.
Fails also on an instruction it does not know, and, so that it cannot pass for want of stubs to look at, if the map declares nothing.

With --print it checks nothing and prints what each stub changes, on paths without a call and on any path, and how else it leaves.

With <jsc> it compiles an empty program and looks at that image. The image is only read, so an executable that holds one can be
given as it is, with the map that was written when it was built (--aotMapFilePath).
"""
import argparse, bisect, collections, os, re, struct, subprocess, sys, tempfile

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('jsc', nargs='?', type=os.path.abspath)
parser.add_argument('--image', type=os.path.abspath)
parser.add_argument('--map', type=os.path.abspath)
parser.add_argument('--print', action='store_true', dest='prints')
parser.add_argument('--stub', action='append', default=[])
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
names, copies, functions, declared = [], [], [], {}
for line in open(args.map, errors='replace'):
    fields = line.rstrip('\n').split('\t')
    if fields[0] == 'T':
        names.append((int(fields[1]), fields[2]))
    elif fields[0] == 'C':
        copies.append(int(fields[1]))
    elif fields[0] == 'F':
        functions.append(int(fields[2]))
    elif fields[0] == 'K':
        declared[fields[1]] = (fields[2].split(), fields[3].split() if len(fields) > 3 else [])
if not names or not copies:
    sys.exit('%s names no stubs' % args.map)
names.sort()
offsets = [offset for offset, _ in names]
size = min([at for at in functions if at > copies[0]] + [code_size]) - copies[0]
words = struct.unpack_from('<%dI' % (size // 4), data, start + code_offset + copies[0])
if 0xd65f03c0 not in words:
    print('not ARM64 code: nothing to check')
    sys.exit(0)

STACK_POINTER, FRAME_POINTER, LINK_REGISTER = 31, 29, 30
float_register = lambda number: 32 + number
CHANGED_BY_CALLS = frozenset(list(range(18)) + [float_register(number) for number in list(range(8)) + list(range(16, 32))])
name_of_register = lambda register: 'x%d' % register if register < 32 else 'd%d' % (register - 32)
register_named = lambda name: {'fp': FRAME_POINTER, 'lr': LINK_REGISTER, 'sp': STACK_POINTER}.get(name.lstrip('%')) or int(name.lstrip('%')[1:]) + (32 if name.lstrip('%')[0] in 'dq' else 0)
signed = lambda value, bits: value - ((value & 1 << bits - 1) << 1)
is_br = lambda word: word & 0xfffffc1f == 0xd61f0000
is_blr = lambda word: word & 0xfffffc1f == 0xd63f0000
is_ret = lambda word: word & 0xfffffc1f == 0xd65f0000
is_brk = lambda word: word & 0xffe0001f == 0xd4200000
is_b = lambda word: word >> 26 == 0x05
is_bl = lambda word: word >> 26 == 0x25


def stub_at(offset):
    index = bisect.bisect_right(offsets, offset) - 1
    return names[index][1], offset - names[index][0]


def table_offset_of_br(index):
    load = words[index - 1]
    if load & 0xffc00000 == 0xf9400000:
        return (load >> 10 & 0xfff) * 8
    if load & 0xffe00c00 == 0xf8400000:
        return signed(load >> 12 & 0x1ff, 9)
    return None


entries_for_exceptions = set()
for (offset, name), (end, _) in zip(names, names[1:]):
    if name in ('OperationVoid', 'OperationValue'):
        entries_for_exceptions.update(table_offset_of_br(index) for index in range(offset // 4, end // 4) if is_br(words[index]))
entries_for_exceptions.discard(None)
if not entries_for_exceptions:
    sys.exit('cannot tell the entry for exceptions: OperationVoid and OperationValue do not jump through the table')


class UnknownInstruction(Exception):
    pass


def effects_of(word):
    """The registers that the instruction writes, and (is load, base, offset, registers, bytes each, write-back) if it accesses memory."""
    destination = word & 31
    if word == 0xd503201f or word & 0xfffff01f == 0xd503301f or is_brk(word):
        return [], None
    if word & 0x1f000000 == 0x10000000:
        return [destination], None
    if word & 0x1c000000 == 0x10000000:
        sets_flags = word & 0x1f800000 == 0x11000000 and word & 0x20000000 or word & 0x1f800000 == 0x12000000 and word & 0x60000000 == 0x60000000
        has_stack_pointer = word & 0x1f800000 in (0x11000000, 0x12000000) and not sets_flags
        return [destination] if destination != 31 or has_stack_pointer else [], None
    if word & 0x0e000000 == 0x0a000000:
        return [destination] if destination != 31 and word & 0x3fe00000 != 0x3a400000 else [], None
    if word & 0x0a000000 == 0x08000000:
        is_float = bool(word & 0x04000000)
        register = float_register if is_float else lambda number: number
        base = word >> 5 & 31
        if word & 0x3a000000 == 0x28000000:
            is_load, mode = bool(word & 0x400000), word >> 23 & 3
            scale = 2 + (word >> 30) if is_float else 2 + (word >> 31)
            offset = signed(word >> 15 & 0x7f, 7) << scale
            registers = [register(destination), register(word >> 10 & 31)]
            return (registers if is_load else []) + ([base] if mode in (1, 3) else []), (is_load, base, 0 if mode == 1 else offset, registers, 1 << scale, offset if mode in (1, 3) else 0)
        is_load = bool(word & 0x00c00000)
        written = [register(destination)] if is_load else []
        scale = 4 if is_float and word & 0x00800000 else word >> 30
        if word & 0x3b000000 == 0x39000000:
            return written, (is_load, base, (word >> 10 & 0xfff) << scale, [register(destination)], 1 << scale, 0)
        if word & 0x3b200000 == 0x38000000:
            mode, offset = word >> 10 & 3, signed(word >> 12 & 0x1ff, 9)
            return written + ([base] if mode in (1, 3) else []), (is_load, base, 0 if mode == 1 else offset, [register(destination)], 1 << scale, offset if mode in (1, 3) else 0)
        if word & 0x3b200c00 == 0x38200800:
            return written, (is_load, base, None, [register(destination)], 1 << scale, 0)
        raise UnknownInstruction
    if word & 0x0e000000 == 0x0e000000:
        if word & 0x5f203c00 == 0x1e202000 or word & 0x5f200c00 == 0x1e200400:
            return [], None
        if word & 0x5f20fc00 == 0x1e200000:
            return [float_register(destination) if word >> 16 & 7 in (2, 3, 7) else destination], None
        return [float_register(destination)], None
    raise UnknownInstruction


Result = collections.namedtuple('Result', 'changed exits problems')
results = {}


def follow(entry, stops_at_calls=False):
    if not stops_at_calls:
        if entry in results:
            return results[entry]
        results[entry] = Result(set(CHANGED_BY_CALLS), collections.Counter(), [])
    changed_at_return, exits, problems, counted = set(), collections.Counter(), [], set()

    def leaves_by(how, index):
        if index not in counted:
            counted.add(index)
            exits[how] += 1

    place = lambda index: '%s+%d' % stub_at(4 * index)
    states, work = {}, [(entry // 4, (frozenset(), 0, frozenset(), None))]
    while work:
        index, state = work.pop()
        if index in states:
            earlier = states[index]
            if earlier[1] != state[1]:
                problems.append('%s is reached with two depths of the stack' % place(index))
                continue
            state = (earlier[0] | state[0], earlier[1], earlier[2] & state[2], earlier[3] if earlier[3] == state[3] else None)
            if state == earlier:
                continue
        states[index] = state
        changed, depth, saved, frame = state
        word = words[index] if 0 <= index < len(words) else None
        leaves = word is None or is_br(word) and table_offset_of_br(index) not in entries_for_exceptions or is_b(word) and not 0 <= index + signed(word & 0x3ffffff, 26) < len(words)
        if leaves:
            if not stops_at_calls:
                leaves_by('tail jump at %s' % place(index), index)
                changed_at_return |= changed | CHANGED_BY_CALLS
            continue
        if is_ret(word):
            if depth:
                problems.append('%s returns with the stack pointer off by %d' % (place(index), depth))
            changed_at_return |= changed - {LINK_REGISTER}
            continue
        if is_brk(word):
            continue
        if is_br(word):
            leaves_by('exception', index)
            continue
        if is_b(word):
            work.append((index + signed(word & 0x3ffffff, 26), state))
            continue
        if is_bl(word) or is_blr(word):
            if stops_at_calls:
                continue
            target = index + signed(word & 0x3ffffff, 26) if is_bl(word) else -1
            if 0 <= target < len(words):
                leaves_by('call of %s' % stub_at(4 * target)[0], index)
                changed = changed | follow(4 * target).changed
            else:
                leaves_by('call', index)
                changed = changed | CHANGED_BY_CALLS
            work.append((index + 1, (frozenset(changed), depth, saved, frame)))
            continue
        if word >> 24 == 0x54 or word & 0x7e000000 == 0x34000000:
            work += [(index + signed(word >> 5 & 0x7ffff, 19), state), (index + 1, state)]
            continue
        if word & 0x7e000000 == 0x36000000:
            work += [(index + signed(word >> 5 & 0x3fff, 14), state), (index + 1, state)]
            continue
        try:
            written, access = effects_of(word)
        except UnknownInstruction:
            problems.append('%s is an instruction I do not know: %08x' % (place(index), word))
            continue
        changed, saved = set(changed), set(saved)
        immediate = (word >> 10 & 0xfff) << (12 if word & 0x400000 else 0)
        if word & 0xff8003ff == 0xd10003ff:
            depth, written = depth - immediate, []
        elif word & 0xff8003ff == 0x910003ff:
            depth, written = depth + immediate, []
        elif word == 0x910003bf:
            if frame is None:
                problems.append('%s takes the stack pointer from a frame pointer I lost track of' % place(index))
            else:
                depth = frame
            written = []
        elif STACK_POINTER in written and not (access and access[1] == STACK_POINTER):
            problems.append('%s writes the stack pointer in a way I do not follow' % place(index))
        if word == 0x910003fd:
            frame = depth
        if access and access[1] == STACK_POINTER and access[2] is not None:
            is_load, _, offset, registers, width, write_back = access
            if write_back and offset:
                depth += write_back
            for number, register in enumerate(registers):
                at = depth + (0 if write_back else offset) + number * width
                if is_load:
                    if (at, register) in saved and width == 8:
                        changed.discard(register)
                    else:
                        changed.add(register)
                    if register == FRAME_POINTER:
                        frame = None
                else:
                    saved = {other for other in saved if other[0] != at}
                    if register not in changed and width == 8:
                        saved.add((at, register))
            if write_back and not offset:
                depth += write_back
        else:
            changed.update(register for register in written if register != STACK_POINTER)
            if access and not access[0] and access[1] in (STACK_POINTER, FRAME_POINTER):
                saved = set()
        work.append((index + 1, (frozenset(changed), depth, frozenset(other for other in saved if other[0] >= depth), frame)))
    result = Result(changed_at_return, exits, problems)
    if not stops_at_calls:
        results[entry] = result
    return result


def show(registers):
    return ' '.join(name_of_register(register) for register in sorted(registers)) or 'nothing'


def base_name_and_own_registers(name):
    own = {register_named(register) for register in re.findall(r' into ([xd]\d+)', name)}
    return re.sub(r' (?:of|into) [xd]\d+| of pair \d+| to \w+', '', name), own


if args.prints:
    for offset, name in names[:-1]:
        if args.stub and base_name_and_own_registers(name)[0] not in args.stub:
            continue
        result = follow(offset)
        print('%s\n    without a call: %s\n    on any path: %s' % (name, show(follow(offset, True).changed), 'all that a call changes' if result.changed >= CHANGED_BY_CALLS else show(result.changed)))
        if result.exits:
            print('    ' + ', '.join('%s (%d)' % exit for exit in result.exits.most_common()))
        for problem in result.problems:
            print('    ' + problem)
    sys.exit(0)

if not declared:
    sys.exit('%s declares no stub: nothing was checked' % args.map)
problems, checked = [], 0
for name in declared:
    if not any(base_name_and_own_registers(other)[0] == name for _, other in names):
        problems.append('there is no stub named %s' % name)
for offset, name in names[:-1]:
    base, own = base_name_and_own_registers(name)
    if base not in declared:
        continue
    checked += 1
    result = follow(offset)
    allowed = {register_named(register) for register in declared[base][0] + (declared[base][1] if ' of ' in name else [])} | own
    problems += ['%s has a %s' % (name, exit) for exit in result.exits if exit.startswith('tail jump')]
    if not any(exit.startswith('tail jump') for exit in result.exits) and result.changed - allowed:
        problems.append('%s changes %s, which it is not declared to change' % (name, show(result.changed - allowed)))
    problems += ['%s: %s' % (name, problem) for problem in result.problems]

seen = set()
for problem in problems:
    if problem not in seen:
        seen.add(problem)
        print(problem)
print('%d stubs declared, %d entries checked: %d problems' % (len(declared), checked, len(seen)))
sys.exit(1 if seen else 0)
