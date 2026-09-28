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


# dump-builtin-descriptions.py > builtin-descriptions.json
#
# To be run by CPython, of the version that this is an implementation of, whenever that changes or a module is added to MODULES.
#
# What is built into Python and written in C says what its arguments are, in __text_signature__, and what it is for, in __doc__. Both are part of the
# language as programs see it: inspect.signature() and help() go by them, and what is said when the arguments are wrong follows from the first. Here
# what is built in is written in C++, and takes both from what this writes. See PythonSignatures.h.
#
# Each entry is [key, kind, signature or null, doc or null]. The key is "type" for a type, "type.name" for what is in the __dict__ of one, and
# "module:name" for a function of a module. The kind is the name of the type that CPython has for it.

import builtins
import json
import sys

# The modules that are written in C++ here.
MODULES = ["builtins", "sys", "math", "time", "posix"]


def generator():
    yield


async def coroutine():
    pass


async def async_generator():
    yield


class Class:
    def method(self):
        pass


class Sequence:
    def __getitem__(self, index):
        raise IndexError


def examples():
    """One of each of the types that have no name in builtins."""
    a_coroutine = coroutine()
    a_coroutine.close()
    an_async_generator = async_generator()
    awaitables = [an_async_generator.__anext__(), an_async_generator.asend(None), an_async_generator.athrow(ValueError)]
    try:
        raise ValueError
    except ValueError as error:
        traceback = error.__traceback__
    yield from [
        generator, generator(), a_coroutine, a_coroutine.__await__(), an_async_generator, Class().method, len, str.join, int.__add__, (1).__add__, type.__dict__["__dict__"],
        type(generator).__dict__["__globals__"], dict.__dict__["fromkeys"], generator.__code__, sys._getframe(), sys._getframe().f_locals, sys, None, NotImplemented, ..., Class.__dict__, traceback,
        {}.keys(), {}.values(), {}.items(), iter([]), reversed([]), iter(()), iter(""), iter("ሴ"), iter(b""), iter(bytearray()), iter(range(1)), iter(range(1 << 100)), iter(set()),
        iter({}), iter({}.values()), iter({}.items()), reversed({}), reversed({}.values()), reversed({}.items()), iter(memoryview(b"")), iter(lambda: 1, 2), iter(Sequence()),
        (lambda x: lambda: x)(1).__closure__[0], list[int], int | str, generator.__code__.co_lines(), generator.__code__.co_positions(), sys.flags, sys.version_info, sys.float_info, sys.int_info, sys.hash_info,
        sys.implementation,
    ]
    yield from awaitables
    for awaitable in awaitables:
        awaitable.close()


types = {}


def add_type(a_type):
    is_heap_type = a_type.__flags__ & (1 << 9)
    if a_type.__name__ in types or (is_heap_type and a_type.__module__ != "sys"):
        return
    types[a_type.__name__] = a_type
    for base in a_type.__bases__:
        add_type(base)


for value in vars(builtins).values():
    if isinstance(value, type):
        add_type(value)
for example in examples():
    add_type(type(example))

entries = []


def text(value):
    return value if isinstance(value, str) else None


for name, a_type in types.items():
    entries.append([name, "type", text(a_type.__text_signature__), text(vars(a_type).get("__doc__"))])
    for attribute, value in vars(a_type).items():
        kind = type(value).__name__
        if kind == "staticmethod":
            value = value.__func__
        if kind in ("wrapper_descriptor", "method_descriptor", "classmethod_descriptor", "builtin_function_or_method", "staticmethod"):
            entries.append([name + "." + attribute, kind, text(value.__text_signature__), text(value.__doc__)])
        elif kind in ("getset_descriptor", "member_descriptor"):
            entries.append([name + "." + attribute, kind, None, text(value.__doc__)])

for name in MODULES:
    module = __import__(name)
    entries.append([name + ":", "module", None, text(module.__doc__)])
    for attribute, value in vars(module).items():
        if type(value).__name__ == "builtin_function_or_method":
            entries.append([name + ":" + attribute, "builtin_function_or_method", text(value.__text_signature__), text(value.__doc__)])

entries.sort()
print("[")
print(",\n".join(json.dumps(entry, ensure_ascii=True) for entry in entries))
print("]")
