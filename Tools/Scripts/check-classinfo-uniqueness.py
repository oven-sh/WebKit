#!/usr/bin/env python3
"""Assert that no two JSC ClassInfo objects share an address in a linked binary.

JavaScriptCore identifies C++ types by comparing ClassInfo (s_info) *pointers*,
so two distinct classes must never share one. That invariant is not expressed
anywhere the compiler can see it: a large family of s_info are defined with a
byte-identical boilerplate initializer

    const ClassInfo Foo::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Foo) };

and when the defining translation unit never takes the address, clang emits the
global as local_unnamed_addr and LTO is free to fold them together. The result
is a silent miscompile of type dispatch -- DFG has inlined ordinary calls as
Boolean(arg), and compiled `new Uint8Array(n)` into an Int8Array allocation --
with no build diagnostic at all.

The per-class workaround is CLASSINFO_KEEP_ADDRESS_UNIQUE() in ClassInfo.h, but
applying it by hand only fixes the collisions somebody already noticed. This
script checks the property directly on the linked output so a regression fails
the build instead of a JS test on one platform months later.

Know what this does and does not prove
--------------------------------------
It is a canary, not a proof.

It can only see a fold that leaves BOTH symbol names pointing at one address,
which is what linker ICF does. An IR-level ConstantMerge during LTO instead
RAUWs and deletes the second global, so one name disappears and what is left
looks like a clean, slightly shorter symbol table. The symbol-count floor below
is the (weak) defence against that.

More importantly it checks the `jsc` shell, and `jsc` is not the artifact that
ships -- Bun links its own binary from the `lib/*.a` this build produces, with
its own linker flags. Nothing in this repository passes `--icf`, so a fold that
only happens under the consumer's link will not reproduce here. What actually
carries across that boundary is CLASSINFO_KEEP_ADDRESS_UNIQUE plus -faddrsig:
the keeper marks the symbol address-significant in the .llvm_addrsig table, and
a downstream `--icf=safe` then refuses to fold it. This script guards the source
invariant; it does not certify the shipped artifact.

Usage:
    check-classinfo-uniqueness.py <binary> [<binary> ...]

Set CLASSINFO_UNIQUENESS_CHECK=0 to skip (for bisecting an unrelated failure).
"""

import os
import re
import subprocess
import sys
from collections import defaultdict

# Itanium mangling for a static data member named s_info, e.g.
# _ZN3JSC12JSFinalObject6s_infoE or, for a template specialization,
# _ZN3JSC33JSGenericTypedArrayViewConstructorINS_12JSUint8ArrayEE6s_infoE.
# Mach-O prepends an extra underscore, hence the optional prefix. LTO renames
# symbols it internalizes by appending a suffix (".llvm.12345", ".lto_priv.0"),
# and dropping those would quietly shrink the check to whatever LTO left alone --
# exactly the symbols least likely to have been folded -- so match them too.
S_INFO_SYMBOL = re.compile(r"^_?_ZN.*6s_infoE(\.[A-Za-z_]+\.\d+)?$")

# --defined-only should already exclude these, but be explicit: 'U'/'u' are
# undefined, 'w' is a weak undefined reference. Everything else that carries an
# address and an s_info name is a ClassInfo -- note the section letter varies by
# object format ('r'/'R' or 'd'/'D' on ELF, 's'/'S' on Mach-O).
UNDEFINED_TYPES = frozenset("Uuvw?")

# A JSCOnly jsc has ~400 ClassInfo, and roughly 70% of them are file-local ('s'
# or 'd', not 'S'/'D') because most classes never export their s_info. Anything
# that drops local symbols -- a partial strip, --discard-locals -- would leave a
# symbol table that still looks plausible while hiding most of what we came to
# compare. Refuse to report success on an obviously truncated table rather than
# print a reassuring "all distinct" over a third of the data.
MINIMUM_EXPECTED_CLASSINFO = 200


def find_nm():
    for candidate in (os.environ.get("NM"), "llvm-nm", "nm"):
        if not candidate:
            continue
        try:
            subprocess.run([candidate, "--version"], capture_output=True, check=True)
            return candidate
        except (OSError, subprocess.CalledProcessError):
            continue
    return None


def demangle(names):
    """Best-effort demangle; falls back to the mangled names.

    An LTO suffix (".llvm.9911") makes the name unmangleable, and those are the
    names a reader most needs to recognise, so strip it first and put it back.
    """
    if not names:
        return {}
    stems = []
    suffixes = []
    for name in names:
        match = S_INFO_SYMBOL.match(name)
        suffix = match.group(1) if match and match.group(1) else ""
        stems.append(name[: len(name) - len(suffix)] if suffix else name)
        suffixes.append(suffix)

    for tool in ("llvm-cxxfilt", "c++filt"):
        try:
            result = subprocess.run(
                [tool], input="\n".join(stems), capture_output=True, text=True, check=True
            )
        except (OSError, subprocess.CalledProcessError):
            continue
        demangled = result.stdout.splitlines()
        if len(demangled) == len(stems):
            return {
                name: pretty + suffix
                for name, pretty, suffix in zip(names, demangled, suffixes)
            }
    return {name: name for name in names}


def collisions_in(nm, binary):
    """Return {address: sorted set of s_info symbol names} for shared addresses."""
    result = subprocess.run(
        [nm, "--defined-only", binary], capture_output=True, text=True
    )
    if result.returncode != 0:
        raise RuntimeError(f"{nm} failed on {binary}: {result.stderr.strip()}")

    by_address = defaultdict(set)
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 3:
            continue
        address, symbol_type, name = fields
        if symbol_type in UNDEFINED_TYPES:
            continue
        if not S_INFO_SYMBOL.match(name):
            continue
        by_address[address].add(name)

    total = sum(len(names) for names in by_address.values())
    if total < MINIMUM_EXPECTED_CLASSINFO:
        raise RuntimeError(
            f"only {total} ClassInfo (s_info) symbols found in {binary}, expected "
            f"at least {MINIMUM_EXPECTED_CLASSINFO} -- the binary is stripped, "
            "partially stripped, or was built without a symbol table, so this "
            "check cannot prove anything. Fix the invocation rather than "
            "ignoring it."
        )

    collisions = {
        address: sorted(names)
        for address, names in by_address.items()
        if len(names) > 1
    }
    return collisions, total


def main(argv):
    if os.environ.get("CLASSINFO_UNIQUENESS_CHECK") == "0":
        print("check-classinfo-uniqueness: skipped (CLASSINFO_UNIQUENESS_CHECK=0)")
        return 0

    binaries = argv[1:]
    if not binaries:
        print(__doc__, file=sys.stderr)
        return 2

    nm = find_nm()
    if nm is None:
        print(
            "check-classinfo-uniqueness: neither llvm-nm nor nm is available",
            file=sys.stderr,
        )
        return 2

    failed = False
    for binary in binaries:
        if not os.path.exists(binary):
            print(f"check-classinfo-uniqueness: no such file: {binary}", file=sys.stderr)
            failed = True
            continue

        try:
            collisions, total = collisions_in(nm, binary)
        except RuntimeError as error:
            print(f"check-classinfo-uniqueness: {error}", file=sys.stderr)
            failed = True
            continue

        if not collisions:
            print(f"check-classinfo-uniqueness: {binary}: {total} ClassInfo, all distinct")
            continue

        failed = True
        every_name = [name for names in collisions.values() for name in names]
        pretty = demangle(every_name)
        merged = sum(len(names) - 1 for names in collisions.values())
        print(
            f"\ncheck-classinfo-uniqueness: FAIL: {binary}: {merged} of {total} "
            f"ClassInfo objects were merged onto {len(collisions)} shared "
            f"address(es).\n",
            file=sys.stderr,
        )
        for address in sorted(collisions):
            print(f"  0x{address}", file=sys.stderr)
            for name in collisions[address]:
                print(f"      {pretty.get(name, name)}", file=sys.stderr)
        print(
            "\nJSC compares ClassInfo by address, so these classes are now "
            "indistinguishable at runtime and type dispatch (jsDynamicCast, the "
            "DFG's constructor recognition, ...) will pick whichever one it "
            "checks first.\n\nFix: add CLASSINFO_KEEP_ADDRESS_UNIQUE(<Class>) "
            "immediately after each s_info definition above, in the same "
            "translation unit. See Source/JavaScriptCore/runtime/ClassInfo.h.\n"
            "To get past this while bisecting something unrelated, set "
            "CLASSINFO_UNIQUENESS_CHECK=0.",
            file=sys.stderr,
        )

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
