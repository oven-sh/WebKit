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


# Makes PythonSocketConstants.h out of Modules/socketmodule.c of CPython: the numbers and the few strings that socket_exec() puts in the module, each on the condition that it is put there on.
#
#     python3 convert-socket-constants.py <the directory that CPython's source is in> > ../PythonSocketConstants.h
#
# The lines are CPython's as they are. What includes the header says what ADD_INT_MACRO(), ADD_INT_CONST() and ADD_STR_CONST() are. Most of the conditions are whether the system's headers define the thing, so each system gets what it
# has. The rest are whether CPython's configure found some header, which nothing here says, so what depends on those is left out: it is all for kinds of address that are not written yet.

import re
import sys

source = open(sys.argv[1] + "/Modules/socketmodule.c").read()

# From after the three are defined, to where what is only for Windows begins, after which there is nothing else.
start = source.index("#ifdef AF_UNSPEC\n    ADD_INT_MACRO(m, AF_UNSPEC);\n")
end = source.index("#ifdef SIO_RCVALL\n", start)
assert "ADD_INT_MACRO" not in re.sub(r"(?s)#ifdef SIO_RCVALL\n.*?#endif /\* _MSTCPIP_ \*/\n", "", re.sub(r"#undef ADD_INT_MACRO", "", source[end:])), "there is more after what is for Windows"
region = source[start:end]

# A condition, or a statement, that goes on to the next line
region = re.sub(r"(?m)^(\s*#[^\n]*)\\\n\s*", r"\1", region)
region = re.sub(r"(ADD_INT_CONST\(m, \"\w+\",)\n\s*", r"\1 ", region)

DIRECTIVE = re.compile(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif|define|undef)\b")
STATEMENT = re.compile(r"\s*(ADD_INT_MACRO\(m, \w+\)|ADD_INT_CONST\(m, \"\w+\", [\w() ,]+\)|ADD_STR_CONST\(m, \"\w+\", \"[\w:-]+\"\));\s*(/\*.*\*/)?\s*$")
depth = 0
lines = []
in_comment = False
for line in region.split("\n"):
    if in_comment:
        in_comment = "*/" not in line
        continue
    stripped = line.strip()
    if not stripped or stripped.startswith("//"):
        continue
    if stripped.startswith("/*"):
        in_comment = "*/" not in stripped
        continue
    directive = DIRECTIVE.match(line)
    if directive:
        depth += {"ifdef": 1, "ifndef": 1, "if": 1, "endif": -1}.get(directive.group(1), 0)
        assert depth >= 0, line
        lines.append(re.sub(r"\s*/\*.*\*/\s*$", "", stripped))
        continue
    assert STATEMENT.match(line), "not understood: " + line
    lines.append("    " + re.sub(r"\s*/\*.*\*/\s*$", "", stripped))
assert not depth and not in_comment
count = sum(1 for line in lines if line.startswith("    ADD_"))
# Nothing has been taken for part of a comment.
assert count == len(re.findall(r"\bADD_(?:INT_MACRO|INT_CONST|STR_CONST)\(m,", region)), count

print("// Made by lib/convert-socket-constants.py out of Modules/socketmodule.c of CPython. It is not to be changed by hand.")
print("// %d things, each on the condition that CPython puts it in the module on." % count)
print()
print("\n".join(lines))
