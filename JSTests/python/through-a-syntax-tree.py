# through-a-syntax-tree.py <program>
#
# Runs a program by way of its syntax tree: the source is made into a tree of objects, the tree is compiled, and what comes of that is run. It is to do what it does when it is run as it is.

import sys


def compiled(path):
    import _ast
    import posix
    descriptor = posix.open(path, posix.O_RDONLY)
    data = b""
    while chunk := posix.read(descriptor, 1 << 16):
        data += chunk
    posix.close(descriptor)
    return compile(compile(data, path, "exec", _ast.PyCF_ONLY_AST), path, "exec")


sys.argv = sys.argv[1:]
__file__ = sys.argv[0]
code = compiled(__file__)
del compiled, sys
exec(globals().pop("code"), globals())
