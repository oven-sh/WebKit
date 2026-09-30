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
# dump-builtin-descriptions.py builtin-descriptions.json > builtin-descriptions-linux.json
#
# To be run by CPython, of the version that this is an implementation of and as it comes, whenever that changes or a module is added to MODULES. The first on macOS, and the second on Linux.
#
# Not everything is the same everywhere: select.epoll is Linux's and select.kqueue is not, and os.sendfile() takes more on macOS. Given what was written on another system, this writes only how this one differs: the entries
# that are not there or are not the same, and [key, null] for what is there and is not here.
#
# What is built into Python and written in C says what its arguments are, in __text_signature__, and what it is for, in __doc__. Both are part of the
# language as programs see it: inspect.signature() and help() go by them, and what is said when the arguments are wrong follows from the first. Here
# what is built in is written in C++, and takes both from what this writes. See PythonSignatures.h.
#
# Each entry is [key, kind, signature or null, doc or null]. The key is "type" for a type, "type.name" for what is in the __dict__ of one, and
# "module:name" for a function of a module. The kind is the name of the type that CPython has for it.
#
# The entry for a type has a fifth item: [__basicsize__, __itemsize__, __dictoffset__, __weakrefoffset__, __flags__]. The numbers mean nothing here,
# where nothing is laid out as in CPython, but programs can see them. And whether they are zero decides what a class derived from it can have: whether
# instances have a __dict__ already, whether there can be weak references to them already, and whether they can be given __slots__.

import _ast
import _contextvars
import _typing
import builtins
import json
import sys

# The modules that are written in C++ here.
MODULES = ["builtins", "sys", "sys._jit", "sys.monitoring", "math", "time", "posix", "_typing", "_contextvars", "_warnings", "_ast", "_weakref", "_thread", "_imp", "marshal", "_io", "_codecs", "errno", "itertools", "_collections", "_sre", "_tokenize", "_opcode", "_string", "atexit", "_signal", "_posixsubprocess", "select", "_random", "_struct", "unicodedata", "binascii", "array", "_abc", "_operator", "_functools", "_heapq", "_bisect", "cmath", "resource", "_symtable", "_csv", "_socket", "_asyncio", "fcntl", "termios"]
# Those whose classes are made when the module is, as a class statement makes one, and are written in C all the same.
MODULES_OF_CLASSES = ("sys", "typing", "_typing", "_thread", "_io", "os", "posix", "resource", "itertools", "collections", "re", "_sre", "_tokenize", "time", "select", "_random", "_struct", "unicodedata", "array", "_abc", "operator", "functools", "_csv", "_socket", "_asyncio", "signal")


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


class Exporter:
    def __buffer__(self, flags):
        return memoryview(b"")


class Finalized:
    attribute = None

    def __delattr__(self, name):
        pass

    def __del__(self):
        raise ValueError


def examples():
    """One of each of the types that have no name in builtins."""
    a_coroutine = coroutine()
    a_coroutine.close()
    an_async_generator = async_generator()
    awaitables = [an_async_generator.__anext__(), an_async_generator.asend(None), an_async_generator.athrow(ValueError), anext(an_async_generator, None)]
    try:
        raise ValueError
    except ValueError as error:
        traceback = error.__traceback__
    unraisable = []
    hook, sys.unraisablehook = sys.unraisablehook, unraisable.append
    del Finalized().attribute
    sys.unraisablehook = hook
    yield from [
        unraisable[0], sys.thread_info, sys.get_asyncgen_hooks(), memoryview(Exporter()).obj, generator, generator(), a_coroutine, a_coroutine.__await__(), an_async_generator, Class().method, len, str.join, int.__add__, (1).__add__, type.__dict__["__dict__"],
        type(generator).__dict__["__globals__"], dict.__dict__["fromkeys"], generator.__code__, sys._getframe(), sys._getframe().f_locals, sys, None, NotImplemented, ..., Class.__dict__, traceback,
        {}.keys(), {}.values(), {}.items(), iter([]), reversed([]), iter(()), iter(""), iter("ሴ"), iter(b""), iter(bytearray()), iter(range(1)), iter(range(1 << 100)), iter(set()),
        iter({}), iter({}.values()), iter({}.items()), reversed({}), reversed({}.values()), reversed({}.items()), iter(memoryview(b"")), iter(lambda: 1, 2), iter(Sequence()),
        (lambda x: lambda: x)(1).__closure__[0], list[int], iter(list[int]), int | str, generator.__code__.co_lines(), generator.__code__.co_positions(), sys.flags, sys.version_info, sys.float_info, sys.int_info, sys.hash_info,
        sys.implementation, t"{1}", iter(t""), t"{1}".interpolations[0], _typing.NoDefault, _typing.TypeVar("T", default=int).evaluate_default,
        _contextvars.Context(), _contextvars.ContextVar("v"), _contextvars.Context().run(_contextvars.ContextVar("v").set, 1), _contextvars.Token.MISSING,
        _contextvars.Context().keys(), _contextvars.Context().values(), _contextvars.Context().items(),
    ]
    yield from awaitables
    for awaitable in awaitables:
        awaitable.close()


types = {}


def add_type(a_type):
    is_heap_type = a_type.__flags__ & (1 << 9)
    if a_type.__name__ in types or (is_heap_type and a_type.__module__ not in MODULES_OF_CLASSES and a_type is not _ast.AST):
        return
    types[a_type.__name__] = a_type
    for base in a_type.__bases__:
        add_type(base)


for value in vars(builtins).values():
    if isinstance(value, type):
        add_type(value)
for name in ("_typing", "_weakref", "_thread", "_io", "posix", "resource", "itertools", "_collections", "_tokenize", "time", "select", "_random", "_struct", "unicodedata", "array", "_operator", "_functools", "_csv", "_socket", "_asyncio", "_signal"):
    for value in vars(__import__(name)).values():
        if isinstance(value, type):
            add_type(value)
for example in examples():
    add_type(type(example))
add_type(type(__import__("_codecs").charmap_build("\0a")))
add_type(type(__import__("_string").formatter_parser("")))
add_type(type(__import__("_string").formatter_field_name_split("")[1]))
add_type(type(__import__("select").poll()))
add_type(type(__import__("_struct").iter_unpack("b", b"")))
add_type(type(__import__("unicodedata")._ucnhash_CAPI))
add_type(type(iter(__import__("array").array("b"))))
add_type(type(__import__("abc").ABC._abc_impl))
add_type(type(__import__("functools").cmp_to_key(len)))


class Loop:
    "As much of an event loop as it takes to make a future and a task"
    def get_debug(self):
        return False

    def call_soon(self, callback, *args, context=None):
        self.callback = callback


loop = Loop()
add_type(type(iter(__import__("_asyncio").Future(loop=loop))))
begun = coroutine()
__import__("_asyncio").Task(begun, loop=loop)._log_destroy_pending = False
add_type(type(loop.callback))
begun.close()
add_type(type(__import__("_symtable").symtable("", "", "exec")))
a_pattern = __import__("re").compile("a")
for example in (a_pattern, a_pattern.match("a"), a_pattern.scanner("a"), __import__("re")._compile_template(a_pattern, "\\g<0>")):
    add_type(type(example))
with __import__("posix").scandir() as entries_of_directory:
    add_type(type(entries_of_directory))
# What is derived from it has nothing of its own that is written in C.
add_type(_ast.AST)

entries = []


def text(value):
    return value if isinstance(value, str) else None


for name, a_type in types.items():
    layout = [a_type.__basicsize__, a_type.__itemsize__, a_type.__dictoffset__, a_type.__weakrefoffset__, a_type.__flags__]
    entries.append([name, "type", text(a_type.__text_signature__), text(vars(a_type).get("__doc__")), layout])
    for attribute, value in vars(a_type).items():
        kind = type(value).__name__
        if kind == "staticmethod":
            value = value.__func__
        if kind in ("wrapper_descriptor", "method_descriptor", "classmethod_descriptor", "builtin_function_or_method", "staticmethod"):
            entries.append([name + "." + attribute, kind, text(value.__text_signature__), text(value.__doc__)])
        elif kind in ("getset_descriptor", "member_descriptor"):
            entries.append([name + "." + attribute, kind, None, text(value.__doc__)])

for name in MODULES:
    module = __import__(name.partition(".")[0])
    for part in name.split(".")[1:]:
        module = getattr(module, part)
    entries.append([name + ":", "module", None, text(module.__doc__)])
    for attribute, value in vars(module).items():
        if type(value).__name__ == "builtin_function_or_method":
            entries.append([name + ":" + attribute, "builtin_function_or_method", text(value.__text_signature__), text(value.__doc__)])
        # Something to read, that is not the module's __doc__: _heapq.__about__
        elif isinstance(value, str) and attribute.startswith("__") and attribute not in ("__name__", "__doc__", "__package__", "__file__"):
            entries.append([name + ":" + attribute, "text", None, text(value)])

# The functions that belong to no module.
for name in ("strict", "ignore", "replace", "xmlcharrefreplace", "backslashreplace", "namereplace", "surrogatepass", "surrogateescape"):
    value = __import__("_codecs").lookup_error(name)
    entries.append([":" + value.__name__, "builtin_function_or_method", text(value.__text_signature__), text(value.__doc__)])

if len(sys.argv) > 1:
    with open(sys.argv[1]) as file:
        elsewhere = {entry[0]: entry for entry in json.load(file)}
    here = {entry[0] for entry in entries}
    entries = [entry for entry in entries if elsewhere.get(entry[0]) != entry] + [[key, None] for key in elsewhere if key not in here]

entries.sort()
print("[")
print(",\n".join(json.dumps(entry, ensure_ascii=True) for entry in entries))
print("]")
