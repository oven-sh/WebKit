# A BytesIO can be written to, cut short and closed while there is a view of it from getbuffer(). CPython raises BufferError: "Existing exports of data: object cannot be re-sized". It can, because a view is
# released the moment that nothing refers to it. Here that would be at the next collection, and programs that are right would fail. A view has no pointer in it, only where it is looking, and looks each time.
from _io import BytesIO


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


b = BytesIO(b"0123456789")
v = b.getbuffer()
t("the view", lambda: (bytes(v), len(v), v.readonly))
t("written by way of the view", lambda: (v.__setitem__(0, 65), v.__setitem__(slice(1, 3), b"BC"), b.getvalue()))
t("written by way of the file", lambda: (b.seek(3), b.write(b"DE"), bytes(v), b.getvalue()))
t("made longer", lambda: (b.seek(0, 2), b.write(b"more"), b.getvalue(), len(v), bytes(v)))
t("another view sees all of it", lambda: bytes(b.getbuffer()))
t("cut short", lambda: (b.truncate(4), b.getvalue()))
t("what the view has of it now", lambda: (len(v), bytes(v)))
t("past the end of it", lambda: v[8])
t("written past the end of it", lambda: v.__setitem__(8, 1))
t("made longer again", lambda: (b.seek(0, 2), b.write(b"xyzxyzxyz"), b.getvalue(), bytes(v)))
t("what was got before is as it was", lambda: [(before, b.seek(0), b.write(b"!"), v.__setitem__(1, 63), before, b.getvalue()) for before in [b.getvalue()]])
t("closed", lambda: (b.close(), b.closed))
t("the view of what is closed", lambda: (len(v), bytes(v)))
t("released", lambda: (v.release(), v.release()))
t("and then", lambda: bytes(v))
w = BytesIO(b"abc")
with w.getbuffer() as view:
    t("in a with", lambda: (w.write(b"XYZW"), bytes(view), w.getvalue()))
t("after it", lambda: (w.write(b"!"), w.getvalue()))
