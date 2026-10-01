# os.path.normpath() and os.path.splitroot(), which ask nothing of the system, so that what they are given need not be fit to be the name of a file
import os, posixpath, pathlib
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", ascii(r))
names = ["", ".", "..", "/", "//", "///", "////", "a", "a/", "a//", "/a", "//a", "///a", "a/b", "a//b", "a/./b", "a/../b", "a/b/..", "a/b/../..", "a/b/../../..", "../a", "../../a", "/..", "/../a", "//..", "//../a", "/./a", "./a", "./", "./.", "a/.", "a/..", "...", "a/.../b", ".a", "a.", "..a", "a..", " ", " / ", "a b/c d",
         "caf\xe9", "\u20ac/\u20ac", "\U0001f600/..", "a\ud800b", "\ud800", "\ud800/..", "\ud800/\udc00", "\udc80", "a/\udcff/..", "a\0b", "\0", "a/\0/..", "\0/..", "a" * 5000, "/".join("a" * 500), "../" * 300, "a/../" * 300, "/" * 5000]
for n in names:
    attempt("normpath(%a)" % n, lambda: os.path.normpath(n) if len(n) < 100 else (len(os.path.normpath(n)), os.path.normpath(n)[:10]))
    attempt("   splitroot", lambda: os.path.splitroot(n) if len(n) < 100 else [len(x) for x in os.path.splitroot(n)])
    try: b = os.fsencode(n)
    except UnicodeEncodeError: continue
    attempt("   of bytes", lambda: (os.path.normpath(b), os.path.splitroot(b)) if len(n) < 100 else len(os.path.normpath(b)))
for b in (b"\xff", b"\xff/..", b"a/\xe9/./b", b"\xed\xa0\x80", b"\x80\x80//\x80"):
    attempt("normpath(%a)" % b, lambda: (os.path.normpath(b), os.path.splitroot(b)))
class P:
    def __init__(self, v): self.v = v
    def __fspath__(self): return self.v
for v in (None, 5, 1.5, [], bytearray(b"a/.."), memoryview(b"a"), P("a/../b"), P(b"a/../b"), P(5), P("\ud800/."), pathlib.PurePosixPath("a/../b"), type("S", (str,), {})("a/./b"), type("B", (bytes,), {})(b"a/./b")):
    attempt("normpath(%s)" % type(v).__name__, lambda: os.path.normpath(v))
    attempt("   splitroot", lambda: os.path.splitroot(v))
attempt("none", lambda: os.path.normpath())
attempt("two", lambda: os.path.normpath("a", "b"))
attempt("what is made of them", lambda: (os.path.join("\ud800", "a"), os.path.basename("a/\ud800"), os.path.dirname("\ud800/a"), os.path.split("\ud800/a"), os.path.splitext("\ud800.x"), os.path.isabs("\ud800"), os.path.commonpath(["\ud800/a", "\ud800/b"]), os.path.relpath("/\ud800/a", "/\ud800")))
attempt("what does ask the system", lambda: (os.path.exists("\ud800"), os.path.isfile("\ud800"), os.path.isdir("a\0b"), os.path.lexists("\ud800")))
attempt("stat", lambda: os.stat("\ud800"))
attempt("open", lambda: open("\ud800"))
print("done")
