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


# Makes PythonUnicodeTypeDatabase.h out of Objects/unicodetype_db.h of CPython.
#
#     python3 convert-unicode-type-database.py <the directory that CPython's source is in> > ../PythonUnicodeTypeDatabase.h
#
# The numbers are left as they are. What is changed is what C++ and this directory call things, and that there are several numbers to a line where CPython has one.
import re
import sys

source = open(sys.argv[1] + "/Objects/unicodetype_db.h").read()
version = re.search(r'#define UNIDATA_VERSION "(.*?)"', open(sys.argv[1] + "/Modules/unicodedata_db.h").read()).group(1)
python = re.search(r'#define PY_MAJOR_VERSION\s+(\d+)\n#define PY_MINOR_VERSION\s+(\d+)', open(sys.argv[1] + "/Include/patchlevel.h").read()).groups()


def block(start, end):
    first = source.index(start)
    return source[first:source.index(end, first) + len(end)]


def cases(text):
    "The labels of a switch, several to a line"
    lines, run = [], []

    def flush():
        row = "   "
        for label in run:
            piece = " case %s:" % label
            if len(row) + len(piece) > 118:
                lines.append(row)
                row = "   "
            row += piece
        if run:
            lines.append(row)
        run.clear()

    for line in text.split("\n"):
        if match := re.match(r"    case (0x[0-9A-F]+):$", line):
            run.append(match.group(1))
        else:
            flush()
            lines.append(line)
    return "\n".join(lines)


records = block("const _PyUnicode_TypeRecord _PyUnicode_TypeRecords[] = {", "\n};")
records = records.replace("const _PyUnicode_TypeRecord _PyUnicode_TypeRecords[] = {", "static constexpr TypeRecord typeRecords[] = {")
records = re.sub(r"\{(-?\d+), (-?\d+), (-?\d+), (\d+), (\d+), (\d+)\}", r"{ \1, \2, \3, \4, \5, \6 }", records)

extended = block("const Py_UCS4 _PyUnicode_ExtendedCase[] = {", "\n};")
rows, row = [], "   "
for number in re.findall(r"\d+", extended[extended.index("{"):]):
    if len(row) + len(number) + 2 > 118:
        rows.append(row)
        row = "   "
    row += " " + number + ","
rows.append(row)
extended = "static constexpr char32_t extendedCase[] = {\n" + "\n".join(rows) + "\n};"

index1 = block("static const unsigned short index1[] = {", "\n};").replace("static const unsigned short index1[]", "static constexpr uint16_t index1[]")
index2 = block("static const unsigned short index2[] = {", "\n};").replace("static const unsigned short index2[]", "static constexpr uint16_t index2[]")
shift = re.search(r"#define SHIFT (\d+)", source).group(1)
numeric = cases(block("double _PyUnicode_ToNumeric(Py_UCS4 ch)\n{", "\n}\n")).replace("double _PyUnicode_ToNumeric(Py_UCS4 ch)\n{", "double toNumeric(char32_t ch)\n{").replace("(double) ", "")
answers = lambda text: text.replace("return 1;", "return true;").replace("return 0;", "return false;")
whitespace = answers(cases(block("int _PyUnicode_IsWhitespace(const Py_UCS4 ch)\n{", "\n}\n")).replace("int _PyUnicode_IsWhitespace(const Py_UCS4 ch)\n{", "bool isWhitespace(char32_t ch)\n{"))
linebreak = answers(cases(block("int _PyUnicode_IsLinebreak(const Py_UCS4 ch)\n{", "\n}\n")).replace("int _PyUnicode_IsLinebreak(const Py_UCS4 ch)\n{", "bool isLineBreak(char32_t ch)\n{"))

# The licence at the top of this file, as a comment of C++'s
mine = open(sys.argv[0]).read()
licence = mine[mine.index("# Copyright"):mine.index("\n\n\n")]
print("/*\n" + "\n".join((" *" + line[1:]).rstrip() for line in licence.split("\n")) + "\n */")
print("""
// lib/convert-unicode-type-database.py made this from Objects/unicodetype_db.h of CPython %s.%s, which its Tools/unicode/makeunicodedata.py makes from version %s of the Unicode Character Database. It is not to be
// changed by hand. The numbers are as they are there. It is for PythonUnicodeType.cpp and nothing else to include.

// The kinds of character that there are
%s

// What a character is in another case, where that is more than one character or is not to be had by adding something to it
%s

// Which kind each character is, in two steps
static constexpr unsigned shift = %s;
%s

%s

// What number a character stands for. -1.0 if none.
%s
// Whether its bidirectional type is WS, B or S, or its category is Zs
%s
// Whether its line break property is BK, CR, LF or NL, or its bidirectional type is B
%s""" % (*python, version, records, extended, shift, index1, index2, numeric, whitespace, linebreak), end="")
