#!/usr/bin/env python3
"""Checks that the stubs of an x86-64 image change no register that they are not declared to change.
The twin of check-stub-clobbers.py, which knows ARM64. The same rules; the decoding is llvm-objdump's, which has to be installed.

  check-stub-clobbers-x86.py --image <image or executable> --map <map>
  check-stub-clobbers-x86.py ... --print [--stub <name>]...
  check-stub-clobbers-x86.py ... --declare '<name>: %rax %r10 ..' ...   (as if the map had that K line: to try a declaration before it is built)

Compiled code keeps values in registers across a call of a stub that is declared to preserve them. The declaration is in the map, one
line for each such stub: K, its name, the registers it may change. A stub that changes another register, on however rare a path,
corrupts a value of its caller.

Follows every path from the entry of each declared stub, and of each thunk that names an operation and jumps to it, through branches
and through jumps to other stubs, and keeps for every register whether it still holds the caller's value. A register that is stored
to the stack (MOV, MOVSD, MOVQ of 8 bytes, PUSH) while it holds the caller's value, and loaded back from that place, holds it again.
So does one that is copied to another register (MOV of 64 bits, MOVQ between the two kinds, MOVAPS | MOVAPD) and copied back while that
one is untouched. A register that was given an address on the stack (LEA from rsp | rbp, a copy of rsp or of such a register) is held to point there for good: a store
through it may hit any saved place, so all of them are forgotten. Of an xmm register the low 64 bits are what counts: compiled code keeps doubles there. A CALL through a register or
memory, or one that leaves the stubs, is a call by the C convention: it changes rax rcx rdx rsi rdi r8 .. r11 and xmm0 .. xmm15. A CALL
of another stub changes what that stub changes, and what is wrong with that stub is wrong with its caller. Only the first copy of the stubs is read.

A path ends
  - in RET: what is changed there has to be declared, and the stack pointer has to be the caller's;
  - in the jump to the entry for exceptions (through the run-time table, at the place OperationVoid and OperationValue jump through);
  - at the entry of a stub that never returns (HandleException, HandleExceptionAtPrologue, ThrowStackOverflow, ThrowStackOverflowAtPrologue: none may hold a RET);
  - in HLT, INT3, UD2;
  - in any other indirect JMP, or a jump that leaves the stubs: a tail jump, after which nothing is known. A declared stub must not have one.
Fails also on an instruction it does not know, and, so that it cannot pass for want of stubs to look at, if nothing is declared.

With --print it checks nothing and prints what each stub changes, on paths without a call and on any path, and how else it leaves.
"""
import argparse, bisect, collections, glob, os, re, shutil, struct, subprocess, sys, tempfile

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--image', type=os.path.abspath, required=True)
parser.add_argument('--map', type=os.path.abspath, required=True)
parser.add_argument('--print', action='store_true', dest='prints')
parser.add_argument('--stub', action='append', default=[])
parser.add_argument('--declare', action='append', default=[])
args = parser.parse_args()


def tool(name):
    found = shutil.which(name) or next(iter(sorted(glob.glob('/opt/homebrew/opt/llvm/bin/' + name) + glob.glob('/usr/lib/llvm-*/bin/' + name), reverse=True)), None)
    if not found:
        sys.exit('no %s' % name)
    return found


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
        declared[fields[1]] = fields[2].split()
for text in args.declare:
    name, _, registers = text.partition(':')
    declared[name.strip()] = registers.split()
if not names or not copies:
    sys.exit('%s names no stubs' % args.map)
names.sort()
offsets = [offset for offset, _ in names]
size = min([at for at in functions if at > copies[0]] + [code_size]) - copies[0]

directory = tempfile.TemporaryDirectory()
raw, elf = os.path.join(directory.name, 'stubs.bin'), os.path.join(directory.name, 'stubs.o')
open(raw, 'wb').write(data[start + code_offset + copies[0]:start + code_offset + copies[0] + size])
subprocess.run([tool('llvm-objcopy'), '-I', 'binary', '-O', 'elf64-x86-64', '--rename-section', '.data=.text,code', raw, elf], check=True)


def disassemble(begin=None):
    command = [tool('llvm-objdump'), '-d', '--no-show-raw-insn', '--x86-asm-syntax=intel', elf]
    if begin is not None:
        command += ['--start-address=%d' % begin, '--stop-address=%d' % min(begin + 4096, size)]
    found = []
    for line in subprocess.run(command, check=True, capture_output=True, text=True).stdout.split('\n'):
        match = re.match(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*(?:<[^>]*>)?\s*$', line)
        if match:
            found.append((int(match.group(1), 16), match.group(2), match.group(3)))
    return found


instructions, previous = {}, {}


def take(found):
    for (at, mnemonic, operands), following in zip(found, found[1:] + [(size, None, None)]):
        if at in instructions:
            break
        instructions[at] = (mnemonic, [operand.strip() for operand in operands.split(',')] if operands else [], following[0])
        previous.setdefault(following[0], at)


take(disassemble())
if not any(mnemonic == 'ret' for mnemonic, _, _ in instructions.values()) or sum(mnemonic == '(bad)' for mnemonic, _, _ in instructions.values()) > len(instructions) // 20:
    if declared:
        sys.exit('not x86-64 code, and the map declares %d stubs: nothing was checked' % len(declared))
    print('not x86-64 code: nothing to check')
    sys.exit(0)


def instruction_at(at):
    if at not in instructions and 0 <= at < size:
        take(disassemble(at))
    return instructions.get(at)


WHOLE = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + ['r%d' % number for number in range(8, 16)]
STACK_POINTER, FRAME_POINTER = 4, 5
registers_by_name = {}
for number, name in enumerate(WHOLE):
    registers_by_name[name] = number
    if number < 8:
        registers_by_name['e' + name[1:]] = registers_by_name[name[1:]] = number
        registers_by_name[name[1] + 'l' if name[2] == 'x' else name[1:] + 'l'] = number
        if name[2] == 'x':
            registers_by_name[name[1] + 'h'] = number
    else:
        for suffix in 'dwb':
            registers_by_name[name + suffix] = number
for number in range(16):
    registers_by_name['xmm%d' % number] = 16 + number
name_of_register = lambda register: WHOLE[register] if register < 16 else 'xmm%d' % (register - 16)
register_named = lambda name: registers_by_name[name.lstrip('%')]
is_whole = lambda operand: operand in WHOLE or operand.startswith('xmm')
CHANGED_BY_CALLS = frozenset([0, 1, 2, 6, 7, 8, 9, 10, 11] + list(range(16, 32)))
WIDTHS = {'byte': 1, 'word': 2, 'dword': 4, 'qword': 8, 'xmmword': 16}

CHANGES_NOTHING = {'cmp', 'test', 'ucomisd', 'ucomiss', 'comisd', 'bt', 'nop', 'lock'}
WRITES_ITS_FIRST_OPERAND = set('''mov movzx movsx movsxd movabs lea add sub and or xor shl shr sar rol ror inc dec neg not bsf bsr popcnt lzcnt tzcnt bswap adc sbb
    movsd movss movq movd movaps movapd movups movupd cvtsi2sd cvtsi2ss cvttsd2si cvttss2si cvtsd2si cvtsd2ss cvtss2sd addsd subsd mulsd divsd sqrtsd addss subss mulss divss
    xorps xorpd andps andpd andnps andnpd orps orpd pxor por pand psrlq psllq pcmpeqd pcmpeqb movmskpd pmovmskb roundsd roundss unpcklpd punpcklqdq minsd maxsd'''.split())
ENDS_THE_PATH = {'hlt', 'int3', 'ud2'}
is_conditional_jump = lambda mnemonic: mnemonic[0] == 'j' and mnemonic != 'jmp'
is_number = lambda operand: re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', operand) is not None


def memory_of(operand):
    """(width, base, has another register, displacement) of an operand in memory."""
    match = re.fullmatch(r'(?:(\w+) ptr )?\[(.*)\]', operand)
    if not match:
        return None
    base, others, displacement = None, False, 0
    for sign, term in re.findall(r'([+-]?)\s*([^\s+-]+)', match.group(2)):
        if is_number(term):
            displacement += -int(term, 0) if sign == '-' else int(term, 0)
        elif term in registers_by_name and base is None and sign != '-':
            base = registers_by_name[term]
        else:
            others = True
    return WIDTHS.get(match.group(1)), base, others, displacement


def stub_at(offset):
    index = bisect.bisect_right(offsets, offset) - 1
    return names[index][1], offset - names[index][0]


def table_offset_of_jump(at):
    """The place in the run-time table that an indirect jump goes through."""
    _, operands, _ = instructions[at]
    direct = memory_of(operands[0])
    if direct:
        return direct[3]
    before = instructions.get(previous.get(at))
    if before and before[0] == 'mov' and before[1][0] == operands[0] and memory_of(before[1][1]):
        return memory_of(before[1][1])[3]
    return None


def addresses_of(name):
    for (offset, other), (end, _) in zip(names, names[1:]):
        if other == name:
            at = offset
            while at < end and instruction_at(at):
                yield at
                at = instructions[at][2]


entries_for_exceptions = set()
for name in ('OperationVoid', 'OperationValue'):
    entries_for_exceptions.update(table_offset_of_jump(at) for at in addresses_of(name) if instructions[at][0] == 'jmp' and not is_number(instructions[at][1][0]))
entries_for_exceptions.discard(None)
if not entries_for_exceptions:
    sys.exit('cannot tell the entry for exceptions: OperationVoid and OperationValue do not jump through the table')

entries_that_never_return = set()
for offset, name in names:
    if name in ('HandleException', 'HandleExceptionAtPrologue', 'ThrowStackOverflow', 'ThrowStackOverflowAtPrologue'):
        if any(instructions[at][0] == 'ret' for at in addresses_of(name)):
            sys.exit('%s is taken never to return and holds a RET' % name)
        entries_that_never_return.add(offset)

Result = collections.namedtuple('Result', 'changed exits problems')
results = {}


def follow(entry, stops_at_calls=False):
    if not stops_at_calls:
        if entry in results:
            return results[entry]
        results[entry] = Result(set(CHANGED_BY_CALLS), collections.Counter(), [])
    changed_at_return, exits, problems, counted = set(), collections.Counter(), [], set()

    def leaves_by(how, at):
        if at not in counted:
            counted.add(at)
            exits[how] += 1

    place = lambda at: '%s+%d' % stub_at(at)
    states, work = {}, [(entry, (frozenset(), 0, frozenset(), None, frozenset(), frozenset()))]
    while work:
        at, state = work.pop()
        if at in states:
            earlier = states[at]
            if earlier[1] != state[1]:
                problems.append('%s is reached with two depths of the stack' % place(at))
                continue
            state = (earlier[0] | state[0], earlier[1], earlier[2] & state[2], earlier[3] if earlier[3] == state[3] else None, earlier[4] & state[4], earlier[5] | state[5])
            if state == earlier:
                continue
        states[at] = state
        changed, depth, saved, frame, kept, points_to_stack = state
        if at in entries_that_never_return and at != entry:
            leaves_by('exception', at)
            continue
        found = instruction_at(at)
        mnemonic, operands, following = found if found else (None, [], None)
        if mnemonic == 'lock' and operands:
            mnemonic, operands = operands[0].split(None, 1)[0], [operands[0].split(None, 1)[1]] + operands[1:]
        goes_to = None
        if mnemonic and (mnemonic in ('jmp', 'call') or is_conditional_jump(mnemonic)):
            goes_to = int(operands[0], 0) if is_number(operands[0]) else None
        leaves = mnemonic is None or mnemonic == 'jmp' and (goes_to is None and table_offset_of_jump(at) not in entries_for_exceptions or goes_to is not None and not 0 <= goes_to < size)
        if leaves:
            if not stops_at_calls:
                leaves_by('tail jump at %s' % place(at), at)
                changed_at_return |= changed | CHANGED_BY_CALLS
            continue
        if mnemonic == 'ret':
            if operands:
                problems.append('%s is a RET that takes bytes off the stack' % place(at))
            if depth:
                problems.append('%s returns with the stack pointer off by %d' % (place(at), depth))
            changed_at_return |= changed
            continue
        if mnemonic in ENDS_THE_PATH:
            continue
        if mnemonic == 'jmp':
            if goes_to is None:
                leaves_by('exception', at)
            else:
                work.append((goes_to, state))
            continue
        if mnemonic == 'call':
            if stops_at_calls:
                continue
            if goes_to is not None and 0 <= goes_to < size:
                leaves_by('call of %s' % stub_at(goes_to)[0], at)
                callee = follow(goes_to)
                changed = changed | callee.changed
                problems += ['behind the call at %s: %s' % (place(at), problem) for problem in callee.problems]
                problems += ['behind the call at %s: a %s' % (place(at), exit) for exit in callee.exits if exit.startswith('tail jump')]
            else:
                leaves_by('call', at)
                changed = changed | CHANGED_BY_CALLS
            work.append((following, (frozenset(changed), depth, saved, frame, frozenset(pair for pair in kept if pair[0] not in changed), points_to_stack)))
            continue
        if is_conditional_jump(mnemonic):
            if goes_to is None or not 0 <= goes_to < size:
                problems.append('%s is a conditional jump that leaves the stubs' % place(at))
            else:
                work += [(goes_to, state), (following, state)]
            continue

        changed, saved, kept = set(changed), set(saved), set(kept)
        written, copy = [], None
        in_memory = [memory_of(operand) for operand in operands]

        def slot_of(access):
            """Where on the stack, from the caller's stack pointer; None: not on the stack; False: on it, I cannot tell where."""
            _, base, others, displacement = access
            if base == STACK_POINTER:
                return False if others else depth + displacement
            if base == FRAME_POINTER and frame is not None:
                return False if others else frame + displacement
            if base in points_to_stack or others and points_to_stack:
                return False
            return None

        def forget(where, width):
            nonlocal saved
            saved = set() if where is False else {other for other in saved if other[0] + 8 <= where or where + (width or 16) <= other[0]}

        if mnemonic in CHANGES_NOTHING:
            pass
        elif mnemonic == 'push':
            depth -= 8
            forget(depth, 8)
            if operands[0] in WHOLE and registers_by_name[operands[0]] not in changed:
                saved.add((depth, registers_by_name[operands[0]]))
        elif mnemonic == 'pop':
            if operands[0] not in WHOLE:
                problems.append('%s is a POP I do not follow' % place(at))
                continue
            register = registers_by_name[operands[0]]
            if (depth, register) in saved:
                changed.discard(register)
            else:
                changed.add(register)
            kept = {pair for pair in kept if pair[0] != register}
            if register == FRAME_POINTER:
                frame = None
            depth += 8
        elif mnemonic in ('mul', 'div', 'idiv') or mnemonic == 'imul' and len(operands) == 1:
            written = [0, 2]
        elif mnemonic in ('cdq', 'cqo'):
            written = [2]
        elif mnemonic == 'imul':
            written = [registers_by_name[operands[0]]]
        elif mnemonic == 'xchg':
            written = [registers_by_name[operand] for operand in operands if operand in registers_by_name]
            for access in in_memory:
                if access and slot_of(access) is not None:
                    forget(slot_of(access), access[0])
        elif mnemonic == 'cmpxchg':
            written = [0] + ([registers_by_name[operands[0]]] if operands[0] in registers_by_name else [])
            if in_memory[0] and slot_of(in_memory[0]) is not None:
                forget(slot_of(in_memory[0]), in_memory[0][0])
        elif mnemonic in WRITES_ITS_FIRST_OPERAND or mnemonic.startswith('cmov') or mnemonic.startswith('set'):
            destination = operands[0]
            if in_memory[0]:
                where = slot_of(in_memory[0])
                if where is not None:
                    forget(where, in_memory[0][0])
                    source = operands[1] if len(operands) > 1 else ''
                    if where is not False and mnemonic in ('mov', 'movsd', 'movq') and in_memory[0][0] == 8 and is_whole(source) and registers_by_name[source] not in changed:
                        saved.add((where, registers_by_name[source]))
            elif destination not in registers_by_name:
                problems.append('%s writes to what I cannot read: %s' % (place(at), destination))
                continue
            else:
                register = registers_by_name[destination]
                if register == STACK_POINTER:
                    if mnemonic in ('add', 'sub') and is_number(operands[1]):
                        depth += int(operands[1], 0) * (1 if mnemonic == 'add' else -1)
                    elif mnemonic == 'mov' and operands[1] == 'rbp' and frame is not None:
                        depth = frame
                    elif mnemonic == 'lea' and in_memory[1] and not in_memory[1][2] and slot_of(in_memory[1]) not in (None, False):
                        depth = slot_of(in_memory[1])
                    else:
                        problems.append('%s writes the stack pointer in a way I do not follow' % place(at))
                        continue
                else:
                    comes_from = in_memory[1] if len(operands) > 1 and mnemonic != 'lea' else None
                    where = slot_of(comes_from) if comes_from else None
                    if mnemonic in ('mov', 'movsd', 'movq') and comes_from and comes_from[0] == 8 and is_whole(destination) and where not in (None, False) and (where, register) in saved:
                        changed.discard(register)
                        kept = {pair for pair in kept if pair[0] != register}
                        if register == FRAME_POINTER:
                            frame = None
                    else:
                        written = [register]
                        if mnemonic in ('mov', 'movq', 'movaps', 'movapd') and len(operands) == 2 and is_whole(destination) and is_whole(operands[1]):
                            copy = (register, registers_by_name[operands[1]])
                    if mnemonic == 'lea' and in_memory[1] and slot_of(in_memory[1]) is not None or len(operands) > 1 and registers_by_name.get(operands[1]) in points_to_stack | {STACK_POINTER}:
                        points_to_stack = points_to_stack | {register}
                    if mnemonic == 'mov' and destination == 'rbp' and operands[1] == 'rsp':
                        frame, copy = depth, None
                    elif register == FRAME_POINTER and register in written:
                        frame = None
        else:
            problems.append('%s is an instruction I do not know: %s %s' % (place(at), mnemonic, ', '.join(operands)))
            continue
        comes_back = copy and (copy[1], copy[0]) in kept
        is_kept = copy and copy[1] not in changed and copy[1] != STACK_POINTER
        changed.update(written)
        kept = {pair for pair in kept if pair[0] not in written}
        if comes_back:
            changed.discard(copy[0])
        elif is_kept:
            kept.add(copy)
        work.append((following, (frozenset(changed), depth, frozenset(other for other in saved if other[0] >= depth), frame, frozenset(kept), points_to_stack)))
    result = Result(changed_at_return, exits, problems)
    if not stops_at_calls:
        results[entry] = result
    return result


def show(registers):
    return ' '.join(name_of_register(register) for register in sorted(registers)) or 'nothing'


base_name = lambda name: re.sub(r' to \w+', '', name)

if args.prints:
    for offset, name in names[:-1]:
        if ' to ' in name and not args.stub or args.stub and base_name(name) not in args.stub or ' to ' in name:
            continue
        result = follow(offset)
        print('%s\n    without a call: %s\n    on any path: %s' % (name, show(follow(offset, True).changed), 'all that a call changes' + (' and ' + show(result.changed - CHANGED_BY_CALLS) if result.changed - CHANGED_BY_CALLS else '') if result.changed >= CHANGED_BY_CALLS else show(result.changed)))
        if result.exits:
            print('    ' + ', '.join('%s (%d)' % exit for exit in result.exits.most_common()))
        for problem in result.problems:
            print('    ' + problem)
    sys.exit(0)

if not declared:
    sys.exit('%s declares no stub: nothing was checked' % args.map)
problems, checked = [], 0
for name in declared:
    if not any(base_name(other) == name for _, other in names):
        problems.append('there is no stub named %s' % name)
for offset, name in names[:-1]:
    base = base_name(name)
    if base not in declared:
        continue
    checked += 1
    result = follow(offset)
    allowed = {register_named(register) for register in declared[base]}
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
