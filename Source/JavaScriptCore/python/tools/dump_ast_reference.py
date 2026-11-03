#!/usr/bin/env python3
# Copyright (C) 2026 Oven, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause
"""Prints what `PYTHON_DUMP_AST=1 jsc file.py...` prints, from CPython's own parser. Run it with CPython 3.15."""
import ast
import struct
import sys

sys.setrecursionlimit(100000)


def constant(v):
    if v is None:
        return "N"
    if v is True:
        return "T"
    if v is False:
        return "F"
    if v is Ellipsis:
        return "E"
    if isinstance(v, str):
        # A str is UTF-16 in JSC: a high surrogate followed by a low one is the character they encode.
        v = v.encode("utf-16-le", "surrogatepass").decode("utf-16-le", "surrogatepass")
        return "s:" + v.encode("utf-8", "surrogatepass").hex()
    if isinstance(v, bytes):
        return "b:" + v.hex()
    if isinstance(v, int):
        return "i:%x" % v
    if isinstance(v, float):
        return "f:" + struct.pack(">d", v).hex()
    if isinstance(v, complex):
        return "c:" + struct.pack(">d", v.real).hex() + "," + struct.pack(">d", v.imag).hex()
    raise TypeError(type(v))


SIMPLE = (ast.expr_context, ast.boolop, ast.operator, ast.unaryop, ast.cmpop)
CONSTANT_FIELDS = {("Constant", "value"), ("MatchSingleton", "value"), ("Interpolation", "str")}


def fmt(owner, name, v):
    if (owner, name) in CONSTANT_FIELDS:
        return constant(v)
    if v is None:
        return "~"
    if isinstance(v, list):
        return "[" + ",".join(fmt(owner, name, x) for x in v) + "]"
    if isinstance(v, SIMPLE):
        return type(v).__name__
    if isinstance(v, ast.AST):
        return node(v)
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return constant(v)
    raise TypeError(type(v))


def node(n):
    name = type(n).__name__
    s = name + "(" + ",".join("%s=%s" % (f, fmt(name, f, getattr(n, f, None))) for f in n._fields) + ")"
    if n._attributes:
        s += "@%d,%d,%d,%d" % (n.lineno, n.col_offset, n.end_lineno, n.end_col_offset)
    return s


for path in sys.argv[1:]:
    print("== " + path)
    with open(path, "rb") as f:
        source = f.read()
    try:
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            tree = ast.parse(source, path)
    except SyntaxError as e:
        print("%s: %s (%r, %r, %r, %r)" % (type(e).__name__, e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset))
        continue
    except ValueError as e:
        print("%s: %s" % (type(e).__name__, e))
        continue
    print(node(tree))
