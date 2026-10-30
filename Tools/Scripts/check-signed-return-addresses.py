#!/usr/bin/env python3
# check-signed-return-addresses.py <binary> [--objdump <llvm-objdump>]
#
# For a lane that is compiled with -mbranch-protection=pac-ret (SIGNED_RETURN_ADDRESSES in .github/scripts/lanes.mjs). It reads the machine code of a finished ARM64 binary that has its
# symbols, because the archives of an LTO lane are bitcode, and checks two things.
#
# The flag has reached the code: a function that saves the link register on the stack signs it first (PACIASP). What no compiler emits never does, and `unsigned_by_nature` names it. In the
# jsc of bun-webkit-linux-arm64-lto-hardened that is 60 functions of the 36,528 that save the link register. Any other function fails the lane: either a part of the build has lost the flag,
# or the function is new and written in assembly, and belongs on the list.
#
# The binary still starts on ARMv8.0: every pointer authentication instruction in it is a hint, which a processor without pointer authentication ignores. RETAA, BLRAA, PACIA and the like
# are undefined instructions there. The compiler emits them once -march says ARMv8.3 or later.
import re, subprocess, sys
a = sys.argv[1:]; binary = a[0]; objdump = 'llvm-objdump'
for i, x in enumerate(a):
    if x == '--objdump': objdump = a[i + 1]
unsigned_by_nature = re.compile('|'.join([
    # llint/*.asm, and the inline assembly of MacroAssemblerARM64.cpp. Not llint_\w+: llint_slow_path_* and the like are C++.
    r'vmEntryTo\w+', r'llint_\w*(?:entry|trampoline|prologue|arity_check)', r'llint_get_host_call_return_value', r'ipint_entry', r'wasm_builtin_trampoline__\w+',
    r'(?:js_to_wasm|wasm_to_js)_wrapper_entry', r'relocateJITReturnPC', r'getSentinelFrameReturnPC', r'enterWebAssemblySuspendingFunction', r'pinballHandler(?:Fulfill|Reject)Function',
    r'ctiMasmProbeTrampoline',
    # The C library's start-up code, libgcc, and what the linker writes.
    r'__do_global_dtors_aux', r'__libc_csu_init', r'init_have_lse_atomics', r'_init', r'_fini', r'\.plt', r'.*@plt.*',
]))
head = re.compile(r'^[0-9a-f]+ <(.*)>:$')
saves = re.compile(r'\t(?:stp|str)\t.*\bx30, \[sp')
not_a_hint = re.compile(r'\t(?:reta[ab]|ereta[ab]|bl?ra[ab]z?|ldra[ab]|(?:pac|aut)[id][ab]|(?:pac|aut)[id]z[ab]|pacga|xpac[id])\b')
name = None; has_saved = has_signed = False; functions = 0; by_nature = 0; unsigned = []; undefined = []
def finish():
    global functions, by_nature
    if not name or not has_saved: return
    functions += 1
    if has_signed: return
    if unsigned_by_nature.fullmatch(name): by_nature += 1
    else: unsigned.append(name)
process = subprocess.Popen([objdump, '-d', '--no-show-raw-insn', binary], stdout=subprocess.PIPE, text=True, errors='replace')
for line in process.stdout:
    line = line.rstrip('\n')
    m = head.match(line)
    if m:
        finish(); name = m.group(1); has_saved = has_signed = False
    elif '\tpaciasp' in line: has_signed = True
    elif saves.search(line): has_saved = True
    elif not_a_hint.search(line): undefined.append(f'{name}: {line.strip()}')
finish()
errors = []
if process.wait(): errors.append(f'{objdump} exited with {process.returncode}, so not all of {binary} was read')
if not functions: errors.append('no function saves the link register: is this an ARM64 binary with its symbols?')
if unsigned:
    errors.append(f'{len(unsigned)} functions save the link register without signing it, and are not written in assembly:\n' + '\n'.join(f'    {name}' for name in unsigned[:200]))
if undefined:
    errors.append(f'{len(undefined)} pointer authentication instructions are not hints, and are undefined before ARMv8.3:\n' + '\n'.join(f'    {line}' for line in undefined[:20]))
print(f'{functions} functions save the link register. {functions - by_nature - len(unsigned)} sign it, and {by_nature} are written in assembly or come with the toolchain.')
for error in errors: print(f'error: {error}')
sys.exit(1 if errors else 0)
