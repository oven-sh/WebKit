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


# Makes PythonUsageText.h out of Python/initconfig.c of CPython: what `python -h`, `--help-env` and `--help-xoptions` print.
#
#     python3 convert-usage-text.py <the directory that CPython's source is in> > ../PythonUsageText.h
#
# The text is CPython's as it is, with the conditions that parts of it are on. All that is changed is that where it is to be filled in with wide characters it is filled in with narrow ones, since what there is here to fill it in
# with is UTF-8.

import re
import sys

source = open(sys.argv[1] + "/Python/initconfig.c").read()

start = source.index("static const char usage_line[] =\n")
# What the last of them is filled in with is defined after it.
end = source.index("#endif\n", source.index("#  define PYTHONHOMEHELP", start)) + len("#endif\n")
region = source[start:end]

names = re.findall(r"(?m)^static const char (\w+)\[\]", region)
assert names == ["usage_line", "usage_help", "usage_xoptions", "usage_envvars"], names
# Nothing but those four, comments, and conditions
rest = re.sub(r"(?s)static const char \w+\[\] =.*?\n;|static const char \w+\[\] =\s*\"[^\n]*\";|static const char \w+\[\] = \"\\\n.*?\";", "", region)
rest = re.sub(r"(?s)/\*.*?\*/", "", rest)
rest = re.sub(r"(?m)^#.*$", "", rest)
assert not rest.strip(), rest.strip()[:200]

region, wide_strings = re.subn(r"%ls", "%s", region)
region, wide_characters = re.subn(r"%lc", "%c", region)
assert (wide_strings, wide_characters) == (1, 2), (wide_strings, wide_characters)
assert region.count("%") == 4, region.count("%")

print("// Made by lib/convert-usage-text.py out of Python/initconfig.c of CPython. It is not to be changed by hand.")
print()
print(region, end="")
