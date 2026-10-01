# What is put in a part of a memoryview has to be of the same kind as what is there, which an array says of itself.
import array
import warnings

# array("u")
warnings.simplefilter("ignore", DeprecationWarning)
def attempt(label, f):
    try: print(label, "=>", f())
    except BaseException as e: print(label, "=>", type(e).__name__, e)
for code in "bBhHiIlLqQfdu":
    try: a = array.array(code, [1, 2, 3, 4] if code != "u" else "abcd")
    except Exception as e: print(code, e); continue
    m = memoryview(a)
    new = array.array(code, [9, 8, 7, 6] if code != "u" else "wxyz")
    attempt(code + " whole", lambda: (m.__setitem__(slice(None), new), a.tolist(), m.format, m.itemsize, memoryview(new).format)[1:])
    attempt(code + " part", lambda: (m.__setitem__(slice(1, 3), new[:2]), a.tolist())[1])
    attempt(code + " from a memoryview", lambda: (m.__setitem__(slice(0, 2), memoryview(new)[2:]), a.tolist())[1])
    attempt(code + " from bytes", lambda: (m.__setitem__(slice(0, 1), bytes(a.itemsize)), a.tolist())[1])
    attempt(code + " wrong length", lambda: m.__setitem__(slice(0, 2), new))
    attempt(code + " another kind", lambda: m.__setitem__(slice(None), array.array("b" if code != "b" else "B", [1, 2, 3, 4])))
    attempt(code + " one", lambda: (m.__setitem__(0, new[1]), a.tolist())[1])
b = bytearray(b"abcd"); mb = memoryview(b)
attempt("bytearray from array B", lambda: (mb.__setitem__(slice(None), array.array("B", [1, 2, 3, 4])), b)[1])
attempt("bytearray from array b", lambda: (mb.__setitem__(slice(None), array.array("b", [1, 2, 3, 4])), b)[1])
attempt("cast", lambda: (memoryview(array.array("i", [1, 2])).cast("B").__setitem__(slice(0, 4), b"\x05\x00\x00\x00")))
