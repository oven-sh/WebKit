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


# Makes PythonCJKCodecs*.cpp and PythonCJKMappings*.h out of Modules/cjkcodecs of CPython.
#
#     python3 convert-cjk-codecs.py <the directory that CPython's source is in> <the directory to write to>
#
# The tables are left as they are. So is what the codecs do, which is written with the macros of cjkcodecs.h: PythonCJKCodecs.h has those. What is changed is what C allows and C++ does not, and that a codec that
# wants a table of another module's has to be told what it is running in to import it.
import re
import sys

source, destination = sys.argv[1] + "/Modules/cjkcodecs/", sys.argv[2] + "/"
python = re.search(r'#define PY_MAJOR_VERSION\s+(\d+)\n#define PY_MINOR_VERSION\s+(\d+)', open(sys.argv[1] + "/Include/patchlevel.h").read()).groups()
licence = open(__file__).read().split("\n\n\n")[0]
licence = "/*\n" + "\n".join((" *" + line[1:]).rstrip() for line in licence.split("\n")[2:]) + "\n */\n"

HEADERS = {
    "mappings_cn.h": "PythonCJKMappingsCN.h", "mappings_hk.h": "PythonCJKMappingsHK.h", "mappings_jp.h": "PythonCJKMappingsJP.h", "mappings_kr.h": "PythonCJKMappingsKR.h", "mappings_tw.h": "PythonCJKMappingsTW.h",
    "mappings_jisx0213_pair.h": "PythonCJKMappingsJISX0213Pair.h", "alg_jisx0201.h": "PythonCJKAlgorithmJISX0201.h", "emu_jisx0213_2000.h": "PythonCJKEmulationJISX0213.h",
}
CODECS = {"cn": "CN", "hk": "HK", "iso2022": "ISO2022", "jp": "JP", "kr": "KR", "tw": "TW"}


def made_from(name):
    return "// lib/convert-cjk-codecs.py made this from Modules/cjkcodecs/%s of CPython %s.%s. It is not to be changed by hand.\n" % (name, *python)


def replace(text, old, new, times):
    "So many times and no other, so that it is noticed if CPython comes to be written otherwise."
    found = len(re.findall(old, text))
    if found != times:
        raise SystemExit("%r is there %d times, and not %d" % (old, found, times))
    return re.sub(old, new, text)


for name, converted in HEADERS.items():
    text = open(source + name).read()
    # Names that begin with two underscores are the compiler's.
    text = re.sub(r"\b__(\w+_(?:decmap|encmap))\b", r"\1_data", text)
    open(destination + converted, "w").write(licence + "\n" + made_from(name) + "// It is for the files that are made with it to include, in a namespace, and nothing else.\n\n" + text)

CHANGES = {
    "cn": [],
    "hk": [(r"cjkcodecs_module_state \*st = codec->modstate;", "cjkcodecs_module_state *st = MODSTATE(codec);", 1)],
    "iso2022": [
        (r"cjkcodecs_module_state \*st = codec->modstate;", "cjkcodecs_module_state *st = MODSTATE(codec);", 5),
        (r"typedef int \(\*iso2022_init_func\)\(const MultibyteCodec \*codec\);", "typedef int (*iso2022_init_func)(JSGlobalObject*, const MultibyteCodec *codec);", 1),
        (r"desig->initializer\(codec\)", "desig->initializer(globalObject, codec)", 1),
        (r"_init\(const MultibyteCodec \*codec\)", "_init(JSGlobalObject* globalObject, const MultibyteCodec *codec)", 5),
        (r"jisx0208_init\(codec\)", "jisx0208_init(globalObject, codec)", 1),
        (r"_PyUnicodeWriter \*writer", "TextWriter *writer", 1),
        (r"_Py_FALLTHROUGH;", "[[fallthrough]];", 1),
        (r"#ifdef Py_DEBUG", "#if ASSERT_ENABLED", 1),
    ],
    "jp": [],
    "kr": [],
    "tw": [],
}

for name, converted in CODECS.items():
    text = open(source + "_codecs_%s.c" % name).read()
    for old, new, times in CHANGES[name]:
        text = replace(text, old, new, times)
    text = replace(text, r'#include "cjkcodecs.h"', '#include "PythonCJKCodecs.h"', 1)
    for old, new in HEADERS.items():
        text = text.replace('#include "%s"' % old, '#include "%s"' % new)
    text = replace(text, r"I_AM_A_MODULE_FOR\(%s\)" % name, "I_AM_A_MODULE_FOR(%s, %s)" % (name, converted), 1)
    # (MultibyteCodec){ ... } is C's.
    text = re.sub(r"NEXT_CODEC = \(MultibyteCodec\)\{(.*?)\};", r"NEXT_CODEC(\1);", text, flags=re.S)
    text = re.sub(r"\bassert *\(", "ASSERT(", text)
    if re.search(r"\bassert\b", text):
        raise SystemExit("there is an assert left in _codecs_%s.c" % name)
    open(destination + "PythonCJKCodecs%s.cpp" % converted, "w").write(licence + "\n" + made_from("_codecs_%s.c" % name) + '\n#include "config.h"\n\n' + text + "\n} } } // namespace JSC::Python::(anonymous)\n")
