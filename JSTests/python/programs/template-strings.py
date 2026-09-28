def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
Template = type(t"")
Interpolation = type(t"{1}".interpolations[0])
name = "World"; n = 3.14159; w = 8

show("empty", lambda: (t"", t"".strings, t"".interpolations, t"".values, list(t"")))
show("only text", lambda: (t"hello", t"hello".strings, list(t"hello")))
show("one", lambda: (t"{name}", t"{name}".strings, t"{name}".values, list(t"{name}")))
show("mixed", lambda: (t"Hello {name}!", t"Hello {name}!".strings, t"Hello {name}!".values))
show("adjacent", lambda: (t"{name}{n}".strings, t"{name}{n}".values, list(t"{name}{n}")))
show("edges", lambda: (t"a{name}".strings, t"{name}b".strings, t"a{name}b{n}c".strings))
show("types", lambda: (Template, Interpolation, type(iter(t"")), Template.__name__, Template.__qualname__, Template.__module__, Interpolation.__module__))
show("interpolation", lambda: (lambda i: (i, i.value, i.expression, i.conversion, i.format_spec))(t"{name}".interpolations[0]))
show("conversions", lambda: [(i.conversion, i.format_spec) for i in t"{name!r}{name!s}{name!a}{name}{n:.2f}{n!r:>10}".interpolations])
show("not applied", lambda: t"{n:.2f}".values)
show("expression text", lambda: [i.expression for i in t"{name}{ name }{name + 'x'}{n*2}{(1, 2)}{[1][0]}{name!r}{n:>{w}}{ n !r}".interpolations])
show("nested specification", lambda: [(i.format_spec, i.value) for i in t"{n:>{w}}{n:{w}.{2}f}{n:{'a'}{'b'}}".interpolations])
show("debug", lambda: (t"{name=}".strings, t"{name=}".interpolations, t"{name = }".strings, t"{name=!s}".interpolations, t"{n=:.1f}".interpolations, t"a{name=}b".strings))
show("braces", lambda: (t"{{}}".strings, t"{{{name}}}".strings, t"{{name}}".strings))
show("escapes", lambda: (t"a\nb\t{name}\x41".strings, rt"a\nb{name}\d".strings, tr"\{name}".strings, Rt"\n".strings, T"{name}".values, tR"\n".strings))
show("quotes", lambda: (t'{name}'.values, t"""a
{name}
b""".strings, t'''{name!r}'''.interpolations, t"{'a' + "b"}".values, t"{name:{"x"}}".interpolations[0].format_spec))
show("multi line expression", lambda: t"""{
    name
}""".interpolations)
show("evaluated in order", lambda: (lambda log: (t"{log.append(1)}{log.append(2)}{log.append(3):{log.append(4)}}", log)[1])([]))
show("evaluated eagerly", lambda: t"{undefined_name}")
show("lambda and others", lambda: (t"{(lambda: 5)()}".values, t"{[i for i in range(3)]}".values, t"{ {'a': 1}['a'] }".values, t"{1 if name else 2}".values, t"{(x := 5)}".values, t"{*[1, 2],}".values))
show("nested templates", lambda: (t"{t"{name}"}".values[0].values, t"{f"{name}!"}".values, f"{t"{name}".values[0]}"))
show("implicit concatenation", lambda: (t"a" t"b", (t"a{name}" t"b{n}").strings, (t"{name}" t"{n}").strings, (t"a" t"" t"c").strings))
show("await in template", lambda: run(co()))
async def co(): return t"{await one()}".values
async def one(): return 1
def run(c):
    try: c.send(None)
    except StopIteration as e: return e.value
show("await in template", lambda: run(co()))
def gen(): yield t"{(yield 1)}"
show("yield in template", lambda: (lambda g: (next(g), g.send("sent").values))(gen()))

# ---- the objects
show("repr", lambda: (repr(t"a{name!r:>5}b"), str(t"a"), repr(t"{name}".interpolations[0])))
show("add", lambda: (t"a" + t"b", (t"a{name}" + t"{n}b").strings, (t"a{name}" + t"{n}b").values, (t"" + t"").strings, (t"{name}x" + t"y{n}").strings))
show("add str", lambda: t"a" + "b")
show("radd str", lambda: "a" + t"b")
show("add other", lambda: t"a" + 1)
show("iadd", lambda: (lambda a: (a.__class__.__name__, a.strings))(iadd()))
def iadd():
    a = t"a"; a += t"{name}"; return a
show("iadd", lambda: iadd().strings)
show("no eq", lambda: (t"a" == t"a", t"{name}".interpolations[0] == t"{name}".interpolations[0], (lambda x: x == x)(t"a"), hash(t"a") != None))
show("no len", lambda: len(t"a"))
show("no getitem", lambda: (lambda x: x[0])(t"a"))
show("no format", lambda: format(t"a", "x"))
show("bool", lambda: (bool(t""), bool(t"a")))
show("no join", lambda: "".join(t"a{name}"))
show("iter", lambda: (lambda i: (iter(i) is i, next(i), next(i), next(i), next(i, "end"), next(i, "end")))(iter(t"a{name}b")))
show("iter skips empty", lambda: [type(x).__name__ for x in t"{name}{n}x{name}"])
show("read only", lambda: setattr(t"a", "strings", ()))
show("read only 2", lambda: setattr(t"{name}".interpolations[0], "value", 1))
show("read only 3", lambda: setattr(t"a", "values", ()))
show("no attributes", lambda: setattr(t"a", "x", 1))
show("delete", lambda: delattr(t"a", "strings"))
show("Template()", lambda: (Template(), Template("a"), Template("a", "b"), Template("a", "b", "c").strings, Template(Interpolation(1)), Template(Interpolation(1), Interpolation(2)).strings, Template("a", Interpolation(1), "b", "c", Interpolation(2)).strings, Template("", "").strings))
show("Template wrong", lambda: Template(1))
show("Template wrong 2", lambda: Template("a", None))
show("Template wrong 3", lambda: Template(t"a"))
show("Template keyword", lambda: Template(strings=()))
class S(str): pass
class Mine: pass
show("Template str subclass", lambda: (Template(S("a")).strings, type(Template(S("a")).strings[0]).__name__, type(Template(S("a"), S("b")).strings[0]).__name__))
show("Template wrong 4", lambda: Template(Mine()))
show("str subclasses elsewhere", lambda: (list(Template(S(""), Interpolation(1), S("x"))), (Template(S("a")) + Template(S("b"))).strings, Interpolation(1, S("e"), S("r"), S("f"))))
show("Interpolation()", lambda: (Interpolation(1), Interpolation(1, "e"), Interpolation(1, "e", "r"), Interpolation(1, "e", None, "f"), Interpolation(value=1, expression="e", conversion="s", format_spec="f"), Interpolation(1, format_spec="x")))
for label, make in [("none", lambda: Interpolation()), ("expression", lambda: Interpolation(1, 2)), ("expression None", lambda: Interpolation(1, None)), ("conversion", lambda: Interpolation(1, "e", "x")), ("conversion type", lambda: Interpolation(1, "e", 1)), ("conversion long", lambda: Interpolation(1, "e", "rr")), ("conversion empty", lambda: Interpolation(1, "e", "")), ("specification", lambda: Interpolation(1, "e", None, 1)), ("too many", lambda: Interpolation(1, "e", None, "", 5)), ("keyword", lambda: Interpolation(1, nope=1)), ("conversion mine", lambda: Interpolation(1, "e", Mine()))]:
    show("Interpolation wrong: " + label, make)
show("cannot subclass", lambda: [cannot(c) for c in (Template, Interpolation, type(iter(t"")))])
def cannot(c):
    try: return type("X", (c,), {})
    except TypeError as e: return str(e)
show("cannot subclass", lambda: [cannot(c) for c in (Template, Interpolation, type(iter(t"")))])
show("cannot make an iterator", lambda: type(iter(t""))())
show("reduce", lambda: t"{name!r:x}".interpolations[0].__reduce__())
show("generic", lambda: (Template[int], Interpolation[str]))
def matches(i):
    match i:
        case Interpolation(v, e, c, f): return (v, e, c, f)
show("match", lambda: (matches(t"{name!r:>5}".interpolations[0]), Interpolation.__match_args__))
show("own attributes", lambda: (sorted(vars(Template)), sorted(vars(Interpolation)), sorted(vars(type(iter(t""))))))
def render(template):
    out = []
    for part in template:
        if isinstance(part, str): out.append(part)
        else:
            v = part.value
            if part.conversion == "r": v = repr(v)
            elif part.conversion == "s": v = str(v)
            elif part.conversion == "a": v = ascii(v)
            out.append(format(v, part.format_spec))
    return "".join(out)
show("rendered as an f-string would be", lambda: (render(t"Hello {name!r:>10}, {n:.2f} and {n:>{w}.1f}!"), f"Hello {name!r:>10}, {n:.2f} and {n:>{w}.1f}!"))
def compile_error(src):
    try: compile(src, "<s>", "exec"); return "compiled"
    except SyntaxError as e: return e.msg
show("errors", lambda: [compile_error(s) for s in ['t"{"', 't"}"', 't"{}"', 't"{a!}"', 't"{a!x}"', 't"a" "b"', '"a" t"b"', 't"a" f"b"', 'f"a" t"b"', 'bt"a"', 'tb"a"', 'ut"a"', 'ft"a"', 'tf"a"', 't"a" b"b"', 't"{a:{b:{c:{d}}}}"', 't"{a!r }"', 't"{a=!}"', "t'{a'", 't"{:x}"', 't"{!r}"', 't"{ }"', 't"{a:}}"', 't"\\N{nope}"']])
from_future = compile('from __future__ import annotations\ndef f(a: t"x{y!r:>{z}}w{{}}"): pass', "<s>", "exec")
ns = {}; exec(from_future, ns)
show("written out again", lambda: ns["f"].__annotations__)
