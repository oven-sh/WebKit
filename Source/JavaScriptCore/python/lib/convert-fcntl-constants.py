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


# Makes PythonFcntlConstants.h out of Modules/fcntlmodule.c of CPython: the numbers that all_ins() puts in the module, each on the condition that it puts it there on.
#
#     python3 convert-fcntl-constants.py <the directory that CPython's source is in> > ../PythonFcntlConstants.h
#
# What includes the header says what ADD_INT_MACRO() is. Nearly all of the conditions are whether the system has the number. HAVE_STROPTS_H is whether CPython's configure found that header, which whoever includes this says.

import re
import sys

source = open(sys.argv[1] + "/Modules/fcntlmodule.c").read()
start = source.index("all_ins(PyObject* m)\n{\n") + len("all_ins(PyObject* m)\n{\n")
end = source.index("    return 0;\n}\n", start)
region = source[start:end]

DIRECTIVE = re.compile(r"#\s*(ifdef|ifndef|if|elif|else|endif|define)\b")
STATEMENT = re.compile(r"if \(PyModule_AddIntMacro\(m, (\w+)\)\) return -1;$")
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
    statement = STATEMENT.match(stripped)
    assert statement, "not understood: " + line
    lines.append("ADD_INT_MACRO(%s);" % statement.group(1))
    count += 1
assert depth == 0
# Every one that there is in the file
assert count == len(re.findall(r"PyModule_Add\w+\(", source)), count

print("// Made by lib/convert-fcntl-constants.py out of Modules/fcntlmodule.c of CPython. It is not to be changed by hand.")
print("// %d numbers, each on the condition that CPython puts it in the module on." % count)
print()
print("\n".join(lines))
