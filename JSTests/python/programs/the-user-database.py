# The modules pwd and grp, and what the library makes of them. Who there is differs from one machine to the next, so nothing is said of anyone but root.
import copy
import getpass
import grp
import os
import pathlib
import pickle
import pwd
import shutil
import tempfile


def t(label, f):
    try:
        print(label, "=>", f())
    except Exception as e:
        print(label, "=>", type(e).__name__, e)


print("---- what there is")
for m in (pwd, grp):
    print(m.__name__, sorted(n for n in vars(m) if not n.startswith("__")), m.__spec__.origin, m.__doc__)
    for n in sorted(vars(m)):
        f = vars(m)[n]
        if callable(f) and not isinstance(f, type):
            print("   ", n, f.__text_signature__, f.__module__, repr(f.__doc__))
for c in (pwd.struct_passwd, grp.struct_group):
    print(c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__[1:]], c.n_fields, c.n_sequence_fields, c.n_unnamed_fields, c.__match_args__, repr(c.__doc__))
    print("   ", [(n, type(vars(c)[n]).__name__, vars(c)[n].__doc__) for n in c.__match_args__])

print("---- root")
root = pwd.getpwuid(0)
print(type(root) is pwd.struct_passwd, isinstance(root, tuple), len(root), root.pw_name, root.pw_uid, root.pw_gid, [type(x).__name__ for x in root], root == pwd.getpwnam("root"), root[0] is root.pw_name, tuple(root) == (root.pw_name, root.pw_passwd, root.pw_uid, root.pw_gid, root.pw_gecos, root.pw_dir, root.pw_shell))
print(repr(root).startswith("pwd.struct_passwd(pw_name='root', pw_passwd="), repr(root).endswith(", pw_shell=%r)" % root.pw_shell), pickle.loads(pickle.dumps(root)) == root, type(pickle.loads(pickle.dumps(root))).__name__, copy.replace(root, pw_name="x").pw_name, hash(root) == hash(tuple(root)))
first = grp.getgrgid(0)
print(type(first) is grp.struct_group, len(first), first.gr_gid, [type(x).__name__ for x in first], first == grp.getgrnam(first.gr_name), all(type(m) is str for m in first.gr_mem), grp.getgrgid(id=0) == first, grp.getgrnam(name=first.gr_name) == first)
print(repr(first).startswith("grp.struct_group(gr_name=%r, gr_passwd=" % first.gr_name), first.gr_mem is not grp.getgrgid(0).gr_mem)

print("---- everyone")
everyone = pwd.getpwall()
print(type(everyone).__name__, len(everyone) > 0, {type(e) for e in everyone} == {pwd.struct_passwd}, root in everyone, all([type(x).__name__ for x in e] == ["str", "str", "int", "int", "str", "str", "str"] for e in everyone))
print(all(pwd.getpwnam(e.pw_name).pw_name == e.pw_name for e in everyone), all(pwd.getpwuid(e.pw_uid).pw_uid == e.pw_uid for e in everyone), pwd.getpwall() == everyone)
groups = grp.getgrall()
print(type(groups).__name__, len(groups) > 0, {type(g) for g in groups} == {grp.struct_group}, all(grp.getgrnam(g.gr_name).gr_name == g.gr_name for g in groups), all(grp.getgrgid(g.gr_gid).gr_gid == g.gr_gid for g in groups), grp.getgrall() == groups)
me = pwd.getpwuid(os.getuid())
print(me.pw_uid == os.getuid(), grp.getgrgid(os.getgid()).gr_gid == os.getgid())

print("---- nobody")
unused = max(e.pw_uid for e in everyone if e.pw_uid < 2 ** 31) + 12345
unused_group = max(g.gr_gid for g in groups if g.gr_gid < 2 ** 31) + 12345
print(str(t.__name__), [type(x).__name__ for x in (unused, unused_group)])
for label, f in (("getpwuid", lambda: pwd.getpwuid(unused).pw_name and None), ("getgrgid", lambda: grp.getgrgid(unused_group).gr_name and None)):
    try:
        f()
    except KeyError as e:
        print(label, type(e).__name__, str(e).replace(str(unused_group), "N").replace(str(unused), "N"), e.__context__)


def found_or_not(f, what):
    "Whether there is anyone by a number that there may be is a matter of where this is run. What the number is taken for is not."
    try:
        return "taken for", f()
    except KeyError as e:
        if not str(e).startswith("'%s not found: " % what):
            raise
        return "taken for", int(str(e)[len(what) + 13:-1])


for value in (-1, -2, -2 ** 31, 2 ** 32 - 1, 2 ** 32, 2 ** 64, -2 ** 64, 1.0, "0", None, b"0", [], True, type("I", (), {"__index__": lambda self: 0})(), type("I", (), {"__index__": lambda self: 2 ** 70})(), type("I", (), {"__index__": lambda self: 1 // 0})(), type("I", (), {"__int__": lambda self: 0})()):
    t("getpwuid(%s)" % repr(value)[:12], lambda: found_or_not(lambda: pwd.getpwuid(value).pw_uid, "getpwuid(): uid"))
    t("getgrgid(%s)" % repr(value)[:12], lambda: found_or_not(lambda: grp.getgrgid(value).gr_gid, "getgrgid(): gid"))
for value in ("no such name as this", "", "é€😀", "a\0b", "\ud800", "\udcff", "x" * 100000, b"root", 0, None, [], pathlib.PurePath("root")):
    t("getpwnam(%s)" % ascii(value)[:24], lambda: pwd.getpwnam(value).pw_uid)
    t("getgrnam(%s)" % ascii(value)[:24], lambda: grp.getgrnam(value).gr_gid)
print("a str of another class", pwd.getpwnam(type("S", (str,), {})("root")).pw_uid, grp.getgrnam(type("S", (str,), {})(first.gr_name)).gr_gid)
for label, f in (("getpwuid()", lambda: pwd.getpwuid()), ("getpwuid(0, 0)", lambda: pwd.getpwuid(0, 0)), ("getpwuid(uidobj=0)", lambda: pwd.getpwuid(uidobj=0)), ("getpwnam()", lambda: pwd.getpwnam()), ("getpwnam(name=)", lambda: pwd.getpwnam(name="root")), ("getpwall(1)", lambda: pwd.getpwall(1)),
                 ("getgrgid()", lambda: grp.getgrgid()), ("getgrgid(0, 0)", lambda: grp.getgrgid(0, 0)), ("getgrgid(gid=0)", lambda: grp.getgrgid(gid=0)), ("getgrnam()", lambda: grp.getgrnam()), ("getgrnam(x=)", lambda: grp.getgrnam(x="")), ("getgrall(1)", lambda: grp.getgrall(1)),
                 ("struct_passwd()", lambda: pwd.struct_passwd()), ("of six", lambda: pwd.struct_passwd(range(6))), ("of seven", lambda: pwd.struct_passwd(range(7))), ("of eight", lambda: pwd.struct_passwd(range(8))), ("struct_group of four", lambda: grp.struct_group("abcd")), ("of three", lambda: grp.struct_group("abc")),
                 ("pw_name = 1", lambda: setattr(root, "pw_name", 1)), ("deriving", lambda: type("D", (pwd.struct_passwd,), {}))):
    t(label, f)

print("---- what the library makes of it")
print(os.path.expanduser("~root") == root.pw_dir, os.path.expanduser("~root/x") == root.pw_dir.rstrip("/") + "/x", os.path.expanduser("~no such name as this/x"), os.path.expanduser(b"~root") == os.fsencode(root.pw_dir))
saved = {n: os.environ.pop(n) for n in ("HOME", "LOGNAME", "USER", "LNAME", "USERNAME") if n in os.environ}
print(os.path.expanduser("~") == me.pw_dir.rstrip("/") or me.pw_dir == "/", getpass.getuser() == me.pw_name, pathlib.Path.home() == pathlib.Path(me.pw_dir))
os.environ.update(saved)
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory, "file")
    path.write_text("x")
    print(path.owner() == me.pw_name, path.group() == grp.getgrgid(path.stat().st_gid).gr_name)
    shutil.chown(path, me.pw_name, grp.getgrgid(path.stat().st_gid).gr_name)
    t("chown to nobody there is", lambda: shutil.chown(path, "no such name as this"))
    t("to no group there is", lambda: shutil.chown(path, group="no such name as this"))
