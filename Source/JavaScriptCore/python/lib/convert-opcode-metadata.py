#!/usr/bin/env python3
#
# Copyright (C) 2026 Apple Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


# Makes PythonOpcodeMetadata.h out of what CPython's headers say about its instructions.
#
#     python3 convert-opcode-metadata.py <the directory that CPython's source is in> > ../PythonOpcodeMetadata.h
#
# It is for the module _opcode. See the README for why there is one, when the instructions are not what is run here.
import re
import sys

root = sys.argv[1]
read = lambda path: open(root + "/" + path).read()
defines = lambda text, pattern: {name: int(value) for name, value in re.findall(r"^#define (%s)\s+(\d+)\s*$" % pattern, text, re.M)}

metadata = read("Include/internal/pycore_opcode_metadata.h")
ids = defines(read("Include/opcode_ids.h"), r"[A-Z][A-Z0-9_]*")
flags = {name: int(value) for name, value in re.findall(r"^#define (HAS_\w+_FLAG) \((\d+)\)$", metadata, re.M)}
count = int(re.search(r"_PyOpcode_opcode_metadata\[(\d+)\]", metadata).group(1))
number = lambda text: int(text) if text.isdigit() else ids[text]


def function(name):
    "What each instruction comes to, by what it comes to"
    text = metadata[metadata.index("int %s(int opcode, int oparg)  {" % name):]
    text = text[:text.index("\n}\n")]
    groups = {}
    for opcode, expression in re.findall(r"case (\w+):\n\s+return (.*?);", text):
        groups.setdefault(expression, []).append(ids[opcode])
    return groups


def switch(name, comment, groups):
    lines = ["// " + comment, "inline int %s(int opcode, [[maybe_unused]] int oparg)" % name, "{", "    switch (opcode) {"]
    for expression, opcodes in groups.items():
        row = "   "
        for opcode in sorted(opcodes):
            piece = " case %d:" % opcode
            if len(row) + len(piece) > 118:
                lines.append(row)
                row = "   "
            row += piece
        lines += [row, "        return %s;" % expression]
    return "\n".join(lines + ["    default:", "        return -1;", "    }", "}"])


table = metadata[metadata.index("const struct opcode_metadata _PyOpcode_opcode_metadata[%d] = {" % count):]
table = table[:table.index("\n};")]
entries = {}
for opcode, is_valid, which in re.findall(r"\[(\w+)\] = \{ (\w+), [-\w]+, (.*?) \},", table):
    entries[ids[opcode]] = (opcode, is_valid, sum(flags[f] for f in which.split(" | ")) if which != "0" else 0)
deopt = metadata[metadata.index("const uint8_t _PyOpcode_Deopt[256] = {"):]
deopt = {number(a): number(b) for a, b in re.findall(r"\[(\w+)\] = (\w+),", deopt[:deopt.index("\n};")])}

rows = []
for opcode in range(count):
    name, is_valid, which = entries.get(opcode, (None, "false", 0))
    rows.append("    { %s, %d, %d }, // %d%s" % (is_valid, which, deopt.get(opcode, 0), opcode, " " + name if name else ""))

intrinsics = read("Include/internal/pycore_intrinsics.h")


def names(pattern, maximum, text):
    found = defines(text, pattern)
    last = found.pop(maximum)
    by_number = {value: name for name, value in found.items()}
    return [by_number[i] for i in range(last + 1)]


unary_last = defines(intrinsics, "MAX_INTRINSIC_1")["MAX_INTRINSIC_1"]
before, after = intrinsics.split("MAX_INTRINSIC_1", 1)
unary = names(r"INTRINSIC_\w+|MAX_INTRINSIC_1", "MAX_INTRINSIC_1", before + "MAX_INTRINSIC_1" + after.split("\n", 1)[0] + "\n")
binary = names(r"INTRINSIC_\w+|MAX_INTRINSIC_2", "MAX_INTRINSIC_2", after.split("\n", 1)[1])
special = names(r"SPECIAL_\w+", "SPECIAL_MAX", read("Include/internal/pycore_ceval.h"))
operators = dict(re.findall(r'ADD_NB_OP\((NB_\w+), "(.*?)"\);', read("Modules/_opcode.c")))
operator_numbers = defines(read("Include/opcode.h"), r"NB_\w+")
last_operator = operator_numbers.pop("NB_OPARG_LAST")
operator_names = {value: name for name, value in operator_numbers.items()}
python = re.search(r"#define PY_MAJOR_VERSION\s+(\d+)\n#define PY_MINOR_VERSION\s+(\d+)", read("Include/patchlevel.h")).groups()
literals = lambda items: "\n".join('    "%s"_s,' % item for item in items)

mine = open(sys.argv[0]).read()
licence = mine[mine.index("# Copyright"):mine.index("\n\n\n")]
print("/*\n" + "\n".join((" *" + line[1:]).rstrip() for line in licence.split("\n")) + "\n */")
print("""
// lib/convert-opcode-metadata.py made this from the headers of CPython %s.%s. It is not to be changed by hand. It is for PythonOpcodeModule.cpp and nothing else to include.

#pragma once

#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace Python { namespace Opcode {

// Include/internal/pycore_opcode_metadata.h
%s

struct Metadata {
    bool isValid;
    uint16_t flags;
    uint8_t deoptimized; // _PyOpcode_Deopt: what it is a special case of, or itself.
};

static constexpr Metadata metadata[] = {
%s
};

%s

%s

// Include/internal/pycore_opcode_utils.h
static constexpr int maximumRealOpcode = %d;
inline bool isBlockPush(int opcode) { return opcode == %d || opcode == %d || opcode == %d; } // SETUP_FINALLY, SETUP_WITH and SETUP_CLEANUP

// Include/internal/pycore_intrinsics.h
static constexpr ASCIILiteral unaryIntrinsics[] = {
%s
};

static constexpr ASCIILiteral binaryIntrinsics[] = {
%s
};

// Include/internal/pycore_ceval.h
static constexpr ASCIILiteral specialMethods[] = {
%s
};

// Include/opcode.h and Modules/_opcode.c
static constexpr std::pair<ASCIILiteral, ASCIILiteral> binaryOperators[] = {
%s
};

} } } // namespace JSC::Python::Opcode""" % (*python,
    "\n".join("static constexpr uint16_t %s = %d;" % ("has" + "".join(p.capitalize() for p in name[4:-5].split("_")), value) for name, value in flags.items()),
    "\n".join(rows),
    switch("numberPopped", "_PyOpcode_num_popped()", function("_PyOpcode_num_popped")),
    switch("numberPushed", "_PyOpcode_num_pushed()", function("_PyOpcode_num_pushed")),
    defines(read("Include/internal/pycore_opcode_utils.h"), "MAX_REAL_OPCODE")["MAX_REAL_OPCODE"], ids["SETUP_FINALLY"], ids["SETUP_WITH"], ids["SETUP_CLEANUP"],
    literals(unary), literals(binary), literals(name[len("SPECIAL_"):].lower() for name in special),
    "\n".join('    { "%s"_s, "%s"_s },' % (operator_names[i], operators[operator_names[i]]) for i in range(last_operator + 1))))
