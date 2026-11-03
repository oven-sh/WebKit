#!/usr/bin/env python3
"""Writes runtime/PyDocstrings.inc: the docstrings of what CPython implements in C and this runtime implements in C++.

    python3.15 tools/make_docstrings.py [file with the host's list of modules ...] > runtime/PyDocstrings.inc

To be run with the CPython whose standard library is shipped. The text is CPython's (PSF license), taken as data: `help(len)`,
`int.__doc__`, and the doctests and tools that read such strings, are to see what they see there.

A key is what the runtime can work out for itself:

    module NAME             the docstring of a module
    type TYPE               that of a type, by the name CPython gives it in C (tp_name): int, collections.deque, _md5.md5 (which
                            is also the name of a function)
    TYPE.NAME               that of a method, slot wrapper, class method, static method, getset or member of the type
    TYPE.__module__         not a docstring: the name of the module, if the type has it in its namespace, where its instances find
                            it too. (Those that CPython makes at run time have: deque().__module__, but not (1).__module__.)
    MODULE.NAME             that of a function of a module

Only modules that the runtime has (runtime/PyNativeModules.cpp, and the files given as arguments, which list modules in the same
way) are looked at, and the types that belong to them.
"""
import importlib
import os
import re
import sys
import types
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
LISTS = [os.path.join(HERE, "..", "runtime", "PyNativeModules.cpp")] + sys.argv[1:]
ALWAYS = ["builtins", "sys", "_imp", "_thread", "_warnings", "_weakref", "marshal", "gc", "posix", "time", "errno", "_io", "_signal", "atexit"]
NATIVE = (types.BuiltinFunctionType, types.MethodDescriptorType, types.WrapperDescriptorType, types.ClassMethodDescriptorType,
          types.GetSetDescriptorType, types.MemberDescriptorType)


def module_names():
    names = set(ALWAYS)
    for path in LISTS:
        with open(path, encoding="utf-8") as f:
            names.update(re.findall(r'\{ "([A-Za-z_0-9.]+)"_s, initialize', f.read()))
    return sorted(names)


def type_name(cls):
    return cls.__qualname__ if cls.__module__ == "builtins" else "%s.%s" % (cls.__module__, cls.__qualname__)


def all_classes():
    seen, stack = set(), [object]
    while stack:
        cls = stack.pop()
        if cls in seen:
            continue
        seen.add(cls)
        try:
            stack.extend(type.__subclasses__(cls))
        except TypeError:
            pass
    return seen


def literal(text):
    out = []
    for byte in text.encode("utf-8"):
        c = chr(byte)
        if c == "\n":
            out.append("\\n")
        elif c in '"\\':
            out.append("\\" + c)
        elif c == "?":
            out.append("\\?")  # No trigraphs.
        elif 32 <= byte < 127:
            out.append(c)
        else:
            out.append("\\%03o" % byte)
    return '"%s"' % "".join(out)


def main():
    warnings.simplefilter("ignore")
    entries = {}

    def add(key, doc):
        if isinstance(doc, str) and doc:
            entries[key] = doc

    names = module_names()
    modules = {}
    for name in names:
        try:
            modules[name] = importlib.import_module(name)
        except ImportError:
            pass
    for name, module in modules.items():
        add("module " + name, module.__doc__)
        for attribute, value in vars(module).items():
            if isinstance(value, types.BuiltinFunctionType) and not isinstance(value.__self__, type):
                add("%s.%s" % (name, value.__name__), value.__doc__)
                add("%s.%s" % (name, attribute), value.__doc__)
                # The functions of _math_integer say that they are of math.integer.
                add("%s.%s" % (value.__module__, value.__name__), value.__doc__)

    for cls in all_classes():
        # What a `class` statement made says so. (The module a type names as its own is often not the one that has it:
        # collections.deque is of _collections.)
        if "__firstlineno__" in vars(cls):
            continue
        members = {}
        for attribute, value in vars(cls).items():
            if isinstance(value, (staticmethod, classmethod)):
                value = value.__func__
            if isinstance(value, NATIVE):
                members[attribute] = value.__doc__
        # A class written in Python has functions, and says so itself what its docstring is.
        if not members and any(isinstance(v, types.FunctionType) for v in vars(cls).values()):
            continue
        name = type_name(cls)
        # What type has under the name is the descriptor that finds the docstrings of all types, and what property has is the
        # one that finds those of its instances.
        doc = vars(cls).get("__doc__")
        add("type " + name, doc if isinstance(doc, str) else cls.__doc__)
        for attribute, doc in members.items():
            add("%s.%s" % (name, attribute), doc)
        add(name + ".__module__", vars(cls).get("__module__"))

    print("// Generated by tools/make_docstrings.py from CPython %d.%d.%d. The text is CPython's." % sys.version_info[:3])
    print("// Sorted by key, as bytes.")
    for key in sorted(entries, key=lambda k: k.encode("utf-8")):
        print("{ %s, %s }," % (literal(key), literal(entries[key])))
    print("// %d entries, %d bytes of text" % (len(entries), sum(len(v.encode("utf-8")) for v in entries.values())), file=sys.stderr)


main()
