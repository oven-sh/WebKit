import m from "./errors.py";
function show(label, f) { try { print(label, "=>", f()); } catch (e) { print(label, "!!", e.name + ":", e.message); } }
function caught(f) { try { f(); } catch (e) { return e; } }
const s = v => JSON.stringify(v);
const frames = e => e.stack.split("\n").map(l => l.replace(/@.*\/([^\/]*)$/, "@$1")).slice(0, 4).join(" | ");

const ke = caught(() => m.outer());
show("is an Error", () => [ke instanceof Error, Error.isError(ke), Object.prototype.toString.call(ke), typeof ke].join(" "));
show("name message", () => [ke.name, ke.message, String(ke)].join(" | "));
show("one stack", () => frames(ke));
show("keys", () => [s(Object.keys(ke)), s(ke), Object.hasOwn(ke, "stack"), "message" in ke, "cause" in ke].join(" "));
show("python class", () => [ke instanceof m.KeyError, ke instanceof m.LookupError, ke instanceof m.Exception, ke instanceof m.ValueError].join(" "));

for (const [py, js] of [["TypeError", TypeError], ["SyntaxError", SyntaxError], ["NameError", ReferenceError], ["ValueError", RangeError], ["RecursionError", RangeError], ["UnicodeDecodeError", RangeError], ["IndentationError", SyntaxError], ["KeyError", RangeError]]) {
    show(py + " instanceof " + js.name, () => Object.create(m[py]) instanceof js);
}
show("raised TypeError", () => { const e = caught(() => m.raiser(m.TypeError, "t")); return [e instanceof TypeError, e instanceof Error, e instanceof RangeError, e.name].join(" "); });
show("derived", () => { const e = caught(() => m.raiser(m.BadType, "b")); return [e instanceof TypeError, e instanceof m.BadType, e.name, e.message].join(" "); });
show("two bases", () => { const e = caught(() => m.raiser(m.Both)); return [e instanceof m.AppError, e instanceof m.ValueError, e.name, e.message, e.code].join(" "); });
show("__str__", () => { const e = caught(() => m.raiser(m.AppError, 7, "seven")); return [e.message, e.code, s(Object.keys(e)), String(e)].join(" | "); });
show("cause", () => { const e = caught(() => m.chained()); return [e.message, e.cause.name, e.cause.message, e.cause instanceof Error].join(" "); });
show("python's own name", () => { const e = caught(() => m.missing_attribute()); return [e.name, e.message, e.__getattribute__("name"), e.obj].join(" | "); });
show("set message", () => { ke.message = "changed"; ke.name = "Renamed"; return [String(ke), s(m.describe(ke).slice(0, 4))].join(" "); });
show("delete message", () => [delete ke.message, delete ke.name, String(ke)].join(" "));
show("attribute from JS", () => { ke.extra = 1; return [s(Object.keys(ke)), s(m.describe(ke)[3])].join(" "); });
show("stack overflow", () => { const e = caught(() => m.recurse()); return [e.name, e instanceof RangeError, e instanceof m.RecursionError].join(" "); });
show("made not raised", () => { const e = m.ValueError("v"); return [e instanceof Error, e.message, frames(e).split(" | ").length > 0, s(m.describe(e).slice(0, 3))].join(" "); });
show("new", () => new m.ValueError("n").message);
show("thrown by JS, caught by Python", () => s(m.catch(() => { throw m.ValueError("round trip"); })));

show("JS TypeError in Python", () => s(m.catch(() => null.x)));
show("JS RangeError in Python", () => s(m.catch(() => new Array(-1))));
show("JS ReferenceError in Python", () => s(m.catch(() => notDefined)));
show("JS SyntaxError in Python", () => s(m.catch(() => JSON.parse("{"))));
show("JS Error in Python", () => s(m.catch(() => { throw new Error("plain"); })));
show("JS subclass in Python", () => s(m.catch(() => { class Mine extends TypeError {} throw new Mine("mine"); })));
show("except TypeError", () => m.catch_as(m.TypeError, () => null.x));
show("describe JS error", () => s(m.describe(new TypeError("js"))));
show("JS error through Python is itself", () => { const e = new RangeError("same"); return caught(() => m.reraise(e)) === e; });
show("Python error through JS is itself", () => { const e = m.KeyError("k"); return caught(() => m.call(() => { throw e; })) === e; });
show("not an Error", () => s(m.catch(() => { throw 42; })));
show("function attributes", () => { m.sample.name; m.sample.length; m.sample.prototype; return s(m.function_attributes(m.sample)); });
