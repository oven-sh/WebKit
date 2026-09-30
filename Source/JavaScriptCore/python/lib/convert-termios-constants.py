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


# Makes PythonTermiosConstants.h out of Modules/termios.c of CPython: the numbers in termios_constants[], each on the condition that it is there on.
#
#     python3 convert-termios-constants.py <the directory that CPython's source is in> > ../PythonTermiosConstants.h
#
# What includes the header says what ADD_CONSTANT() is. All of the conditions are whether the system has the number.

import re
import sys

source = open(sys.argv[1] + "/Modules/termios.c").read()
start = source.index("} termios_constants[] = {\n") + len("} termios_constants[] = {\n")
end = source.index("    /* sentinel */\n    {NULL, 0}\n", start)
region = source[start:end]

DIRECTIVE = re.compile(r"#\s*(ifdef|ifndef|if|elif|else|endif)\b")
ENTRY = re.compile(r'\{\s*"(\w+)", (?:\(long\))?(\w+)\},$')
depth = 0
count = 0
lines = []
in_comment = False
for line in region.split("\n"):
    stripped = line.strip()
    if in_comment:
        in_comment = "*/" not in stripped
        continue
    if not stripped or stripped.startswith("//"):
        continue
    if stripped.startswith("/*"):
        in_comment = "*/" not in stripped
        continue
    directive = DIRECTIVE.match(stripped)
    if directive:
        depth += {"ifdef": 1, "ifndef": 1, "if": 1, "endif": -1}.get(directive.group(1), 0)
        assert depth >= 0, line
        lines.append(re.sub(r"\s*/\*.*\*/\s*$", "", stripped))
        continue
    entry = ENTRY.match(stripped)
    assert entry and entry.group(1) == entry.group(2), "not understood: " + line
    lines.append("ADD_CONSTANT(%s);" % entry.group(1))
    count += 1
assert depth == 0
# Every one that there is in the table
assert count == len(re.findall(r'(?m)^\s*\{\s*"', region)), count

print("// Made by lib/convert-termios-constants.py out of Modules/termios.c of CPython. It is not to be changed by hand.")
print("// %d numbers, each on the condition that CPython has it in the module on." % count)
print()
print("\n".join(lines))
