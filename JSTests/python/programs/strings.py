s = "Hello, World"
print(s.upper(), s.lower(), s[0], s[-1], s[1:4], s[::-1], s[::2], s[-5:], len(s), s * 2, s + "!", "lo" in s, "x" not in s)
print(s.split(", "), s.split("o"), "a b  c".split(), " a b ".split(" "), "a,b,c".split(",", 1), "a,b,c".rsplit(",", 1), "l1\nl2\n".splitlines())
print(s.replace("l", "L"), s.replace("l", "L", 2), s.find("o"), s.find("z"), s.rfind("o"), s.index("W"), s.count("l"), s.startswith("Hell"), s.endswith(("x", "ld")))
print("  pad ".strip(), "  pad ".lstrip() + "|", "  pad ".rstrip() + "|", "xxhixx".strip("x"), "-".join(["a", "b", "c"]), "".join(str(i) for i in range(5)))
print("abc".capitalize(), "hello world".title(), "aBc".swapcase(), "42".isdigit(), "a1".isalnum(), "ab".isalpha(), " ".isspace(), "AB".isupper(), "42".zfill(5), "-42".zfill(5))
print("ab".center(6, "*"), "ab".ljust(5) + "|", "ab".rjust(5), "a=b=c".partition("="), "prefix_x".removeprefix("prefix_"), "x.py".removesuffix(".py"))
print(repr("it's"), repr('say "hi"'), repr("both ' and \""), repr("tab\tnl\n"), repr("\\"), repr(""), str(None), str(True), str([1, "a", None]), repr(("x",)))
print("{} {} {}".format(1, "two", 3.0), "{0}{1}{0}".format("a", "b"), "{x}-{y}".format(x=1, y=2), "{:>5}|{:<5}|{:^5}".format("a", "b", "c"), "{{}}".format(), "{!r}".format("q"))
print("a" < "b", "a" < "B", "abc" < "abd", "ab" < "abc", "" < "a", "a" == "a", "a" != "b", max("hello"), min("hello"), sorted("hello"))
print(ord("a"), chr(97), ord("é"), chr(233), list("abc"), tuple("ab"), [c for c in "hi"], "abc"[1], "%s and %s" % ("x", "y"))
# code points, not UTF-16 units
e = "a😀b"
print(len(e), e[1], e[2], e[::-1], list(e), e[1:], ord(e[1]), e.upper(), len("😀" * 3), "😀" in e, e.index("b") if False else "skip", "\U0001F600" == "😀", "z" < "😀", "￿" < "😀")
name, n, x = "world", 42, 3.14159
print(f"hi {name}! {n + 1} {x:.2f} {name!r} {name:>8} {n:04d} {{literal}} {'nested'} {[i for i in range(3)]} {n=}")
for bad in (lambda: "abc"[5], lambda: "abc".index("z"), lambda: "a" + 1, lambda: "a" * "b", lambda: ",".join([1]), lambda: str("abc")["x"], lambda: 1 in "abc"):
    try: bad()
    except Exception as ex: print(type(ex).__name__, ex)
