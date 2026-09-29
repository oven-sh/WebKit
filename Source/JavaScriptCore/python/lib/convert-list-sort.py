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




# Makes PythonListSortKernel.h out of Objects/listobject.c of CPython: the part of list.sort() that has to do with the order of things and not with what they are.
#
#     python3 convert-list-sort.py <the directory that CPython's source is in> > ../PythonListSortKernel.h
#
# How many times things are compared, and which with which, and in what order, is something that a program can see. It follows from every line of this, and there is no test that is sure to find a line that has been copied out
# wrongly. So it is not copied out by hand. What is changed is what C++ requires, and what stands for an object.
import re
import sys

directory = sys.argv[1]
source = open(directory + "/Objects/listobject.c").read()
python = re.search(r'#define PY_MAJOR_VERSION\s+(\d+)\n#define PY_MINOR_VERSION\s+(\d+)', open(directory + "/Include/patchlevel.h").read()).groups()


def function(name):
    "All of a function, with the comment before it"
    match = re.search(r"(?:/\*(?:[^*]|\*(?!/))*\*/\n)?static \w+\n%s\(.*?\n}\n" % name, source, re.S)
    return match.group(0)


first = source.index("/* Lots of code for an adaptive, stable, natural mergesort.")
last = source.index("/* This struct holds the comparison function and helper functions")
text = function("reverse_slice") + "\n" + source[first:last]

# There is a place in the state for what compares things to find whatever it needs, and for it to be said that there is no more memory.
text = text.replace("""    int (*tuple_elem_compare)(PyObject *, PyObject *, MergeState *);
};""", """    int (*tuple_elem_compare)(PyObject *, PyObject *, MergeState *);

    /* Not in CPython: for the three above. */
    void *context;
    /* Not in CPython: what PyErr_NoMemory() is here. */
    void (*no_memory)(MergeState *);
};""")
assert "void *context;" in text
# It is a class of Python's that says how things of it are compared, and that is for what compares them to keep.
text, count = re.subn(r"    /\* This function is used by unsafe_object_compare to optimize comparisons\n.*?\n    PyObject \*\(\*key_richcompare\)\(PyObject \*, PyObject \*, int\);\n\n", "", text, flags=re.S)
assert count == 1
# C++ does not let `goto fail` go past where something is declared and given a value at once. It may go past where it is only declared.
assert text.count("    Py_ssize_t neq = 0;\n") == 1
text = text.replace("    Py_ssize_t neq = 0;\n", "    Py_ssize_t neq;\n    neq = 0;\n")
text = text.replace("PY_SSIZE_T_MAX", "std::numeric_limits<ptrdiff_t>::max()")
text = text.replace("PyErr_NoMemory();", "ms->no_memory(ms);")
text = re.sub(r"\(PyObject \*\*\)PyMem_Malloc\((.*?)\);", r"static_cast<Item*>(tryAllocate(\1));", text, flags=re.S)
text = text.replace("PyMem_Free(", "release(")
text = re.sub(r"Py_LOCAL_INLINE\((\w+)\)", r"static inline \1", text)
text = text.replace("sizeof(PyObject *)", "sizeof(Item)").replace("sizeof(PyObject*)", "sizeof(Item)")
text = re.sub(r"PyObject \*\*\s*", "Item* ", text)
text = re.sub(r"PyObject \*\s*", "Item ", text)
text = re.sub(r"\bPy_ssize_t\b", "ptrdiff_t", text)
text = re.sub(r"\bassert\(", "ASSERT(", text)
text = re.sub(r"\bNULL\b", "nullptr", text)
text = text.replace("SIZEOF_SIZE_T", "sizeof(size_t)")
text = text.replace("typedef struct {\n    Item* keys;\n    Item* values;\n} sortslice;", "struct sortslice {\n    Item* keys;\n    Item* values;\n};")
text = text.replace("typedef struct s_MergeState MergeState;\nstruct s_MergeState {", "struct MergeState {")
text = text.replace("struct s_slice pending[", "s_slice pending[")
left = re.findall(r"\b_?P[Yy]_?[A-Z]\w*", re.sub(r"/\*.*?\*/", "", text, flags=re.S))
assert not left, left
text = re.sub(r"[ \t]+$", "", text, flags=re.M).strip("\n")

# What is defined here and is wanted no further on
defined = [name for name in dict.fromkeys(re.findall(r"^#define (\w+)", text, re.M)) if not re.search(r"^#undef %s$" % name, text, re.M)]

license = open(sys.argv[0]).read().split("\n\n\n")[0].split("\n", 2)[2]
print("/*\n" + "\n".join((" *" + line[1:]).rstrip() for line in license.split("\n")) + "\n */\n")
print("// This is made by lib/convert-list-sort.py from Objects/listobject.c of CPython %s.%s. It is not to be changed by hand. It is for PythonListSort.cpp and nothing else to include." % python)
print("// Objects/listsort.txt of CPython says how it works, and why.\n")
print("#pragma once\n")
print("#include <wtf/FastMalloc.h>\n")
print("WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN\n")
print("namespace JSC { namespace Python { namespace ListSortKernel {\n")
print("// What is sorted. All that is done with one here is to move it about, and to hand it to what compares it with another.")
print("using Item = EncodedJSValue;\n")
print("static inline void* tryAllocate(size_t size)\n{\n    void* result;\n    return tryFastMalloc(size).getValue(result) ? result : nullptr;\n}\n")
print("static inline void release(void* memory) { fastFree(memory); }\n")
print(text)
print()
for name in defined:
    print("#undef " + name)
print("\n} } } // namespace JSC::Python::ListSortKernel\n")
print("WTF_ALLOW_UNSAFE_BUFFER_USAGE_END")
