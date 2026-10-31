#!/usr/bin/env python3
# check-landing-pads.py <binary>
#
# For a lane with LANDING_PADS (.github/scripts/lanes.mjs). It reads the machine code of a finished ARM64 binary that has its symbols.
#
# JavaScriptCore guards the pages of LLInt, where an indirect branch then traps unless it lands on a landing pad. Every label of LLInt
# but a local one may be where one lands, so offlineasm puts a landing pad behind each (asm.rb), and this checks that it is there: a
# label without one is found by a test only if the test gets there, on a processor that has the feature. What the JITs generate is not
# in the binary: --validateIndirectBranchTargets checks that as it is generated.
import re
import subprocess
import sys

binary = sys.argv[1]
objdump = 'llvm-objdump'
nm = 'llvm-nm'

# What LLInt::initialize() guards is between the bounds of these (LowLevelInterpreter.cpp).
sections = ['jsc_llint', 'jsc_llint_bytecode', 'jsc_llint_webassembly']
instruction = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
# What signs the return address is a landing pad for a call, and for a jump that has its target in x16 or x17.
landing_pad = re.compile(r'bti\s+(?:c|j|jc)|paci[ab]sp')
# The handlers of IPInt are in slots, and such a label is where a slot begins. The jump from one to the next has its target in any
# register (arm64LowerJumpsForLandingPads in arm64.rb), and nothing calls one.
landing_pad_of_slot = re.compile(r'bti\s+j')

errors = []


def lines_of(command):
    process = subprocess.run(command, stdout=subprocess.PIPE, text=True, errors='replace')
    if process.returncode:
        errors.append(f'{command[0]} exited with {process.returncode}, so not all of {binary} was read')
    return process.stdout.splitlines()


# Not from the disassembly, which has one name for an address.
symbols = []
for line in lines_of([nm, binary]):
    parts = line.split()
    if len(parts) == 3:
        symbols.append((int(parts[0], 16), parts[2]))
address_of = {name: address for address, name in symbols}
bounds = {f'{section}_{bound}' for section in sections for bound in ('begin', 'end')}

labels = 0
slots = 0
without = []
for section in sections:
    if f'{section}_begin' not in address_of or f'{section}_end' not in address_of:
        errors.append(f'{section} has no bounds: is this an ARM64 binary with its symbols, built with ENABLE(BRANCH_TARGET_IDENTIFICATION)?')
        continue
    begin, end = address_of[f'{section}_begin'], address_of[f'{section}_end']
    instructions = {}
    for line in lines_of([objdump, '-d', '--no-show-raw-insn', f'--start-address={begin:#x}', f'--stop-address={end:#x}', binary]):
        m = instruction.match(line)
        if m:
            instructions[int(m.group(1), 16)] = m.group(2).strip()
    for address, name in symbols:
        # $x and $d are the assembler's own, for where code and data begin.
        if not begin <= address < end or name in bounds or name.startswith('$'):
            continue
        labels += 1
        is_slot = name.endswith('_validate')
        slots += is_slot
        # The labels of one place share the landing pad.
        if not (landing_pad_of_slot if is_slot else landing_pad).fullmatch(instructions.get(address, 'nothing')):
            without.append(f'{name}: {instructions.get(address, "nothing")}')

if not labels:
    errors.append('LLInt has no label')
if not slots:
    errors.append('LLInt has no slot: are the labels of IPInt\'s handlers still called _validate?')
if without:
    errors.append(f'{len(without)} labels of LLInt do not have the landing pad that they need behind them:\n' + '\n'.join(f'    {line}' for line in without[:200]))
print(f'LLInt has {labels} labels, {slots} of them slots. {labels - len(without)} have the landing pad that they need behind them.')
for error in errors:
    print(f'error: {error}')
sys.exit(1 if errors else 0)
