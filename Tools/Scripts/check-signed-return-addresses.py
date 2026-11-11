#!/usr/bin/env python3
# check-signed-return-addresses.py <binary>
#
# For a lane with SIGNED_RETURN_ADDRESSES (.github/scripts/lanes.mjs). It reads the machine code of a finished ARM64 binary that has its symbols, because the archives of an LTO lane are
# bitcode, and checks two things.
#
# A function that saves the link register on the stack signs it first: PACIASP in what the compiler generates, PACIBSP in LLInt. Any other function fails the lane, unless
# `unsigned_by_nature` names it: either a part of the build has lost the flag, or the function is new and belongs on the list. What the JITs generate is not in the binary:
# JSTests/stress/bun-return-address-of-function-is-signed.js asks the frames.
#
# The binary still starts on ARMv8.0: every pointer authentication instruction in it is a hint, which a processor without pointer authentication ignores. RETAA, BLRAA, PACIA and the like
# are undefined instructions there. The compiler emits them once -march says ARMv8.3 or later.
import re
import subprocess
import sys

binary = sys.argv[1]
objdump = 'llvm-objdump'

unsigned_by_nature = re.compile('|'.join([
    # Whatever jumps to one of these has signed the link register: jsTrampoline*Prologue and ipint_trampoline, or the thunk that takes their place with the JIT.
    r'llint_\w+_(?:prologue|arity_check)', r'ipint_entry',
    # The inline assembly of MacroAssemblerARM64.cpp.
    r'ctiMasmProbeTrampoline',
    # The C library's start-up code, libgcc, and what the linker writes.
    r'__do_global_dtors_aux', r'__libc_csu_init', r'init_have_lse_atomics', r'_init', r'_fini', r'\.plt', r'.*@plt.*',
]))
head = re.compile(r'^[0-9a-f]+ <(.*)>:$')
# Any store, though the compiler saves it with STP and STR alone. Either operand of an STP: without a frame pointer that is `stp x30, x19, [sp, #-16]!`.
saves = re.compile(r'\tst\w+\t(?:[wx]\d+, )?x30, (?:x\d+, )?\[sp')
signs = re.compile(r'\tpaci[ab]sp$')
pointer_authentication = re.compile(r'\t((?:pac|aut|xpac|e?reta|bl?ra[ab]|ldra[ab])\w*)')
# All there are. Whatever else the pattern above finds is taken for undefined before ARMv8.3, also what is newer than this list.
hints = {'paciasp', 'pacibsp', 'paciaz', 'pacibz', 'pacia1716', 'pacib1716', 'autiasp', 'autibsp', 'autiaz', 'autibz', 'autia1716', 'autib1716', 'xpaclri', 'pacm'}

functions = 0
by_nature = 0
unsigned = []
undefined = []
name = None
has_saved = has_signed = has_saved_unsigned = False


def finish():
    global functions, by_nature
    if not name or not has_saved:
        return
    functions += 1
    if not has_saved_unsigned:
        return
    if unsigned_by_nature.fullmatch(name):
        by_nature += 1
    else:
        unsigned.append(name)


process = subprocess.Popen([objdump, '-d', '--no-show-raw-insn', binary], stdout=subprocess.PIPE, text=True, errors='replace')
for line in process.stdout:
    line = line.rstrip('\n')
    m = head.match(line)
    if m:
        finish()
        name = m.group(1)
        has_saved = has_signed = has_saved_unsigned = False
    elif signs.search(line):
        has_signed = True
    elif saves.search(line):
        has_saved = True
        # Signing behind the save is of no use to the word that was saved.
        if not has_signed:
            has_saved_unsigned = True
    else:
        m = pointer_authentication.search(line)
        if m and m.group(1) not in hints:
            undefined.append(f'{name}: {line.strip()}')
finish()

errors = []
if process.wait():
    errors.append(f'{objdump} exited with {process.returncode}, so not all of {binary} was read')
if not functions:
    errors.append('no function saves the link register: is this an ARM64 binary with its symbols?')
if unsigned:
    errors.append(f'{len(unsigned)} functions save the link register without signing it, and are not on the list of those that need not:\n' + '\n'.join(f'    {name}' for name in unsigned[:200]))
if undefined:
    errors.append(f'{len(undefined)} pointer authentication instructions are not hints, and are undefined before ARMv8.3:\n' + '\n'.join(f'    {line}' for line in undefined[:20]))
print(f'{functions} functions save the link register. {functions - by_nature - len(unsigned)} sign it, and {by_nature} are on the list of those that need not.')
for error in errors:
    print(f'error: {error}')
sys.exit(1 if errors else 0)
