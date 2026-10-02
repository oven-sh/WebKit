//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function reads(o) { return o.a + o.b + o.c.d + o.missing + o.toString.name + o.length; }
check(reads({ a: 1, b: 2, c: { d: 3 }, length: "!" }), "NaNtoString!", "reads");
function stores(o) { o.a = 1; o.b = 2; o.a = 3; o["c"] = 4; return o; }
check(JSON.stringify(stores({})), '{"a":3,"b":2,"c":4}', "stores");
function makes(x) { return [{ x, y: 2 }, [x, 3], () => x, function named() { return x; }, class { m() { return x; } }, /r/g, `t${x}`, new Map([[x, x]])]; }
{ let m = makes(7); check(m[0].x + m[0].y + m[1][1] + m[2]() + m[3]() + new m[4]().m() + m[5].flags + m[6] + m[7].get(7), "33gt77", "what it makes"); }
function Constructor(a, b) { this.a = a; this.b = b; }
Constructor.prototype.sum = function () { return this.a + this.b; };
function constructs() { return new Constructor(1, 2).sum(); }
check(constructs(), 3, "constructs");
function globals() { return typeof Math + typeof undefinedThing + typeof check + (globalThis === this0); } var this0 = this;
check(globals(), "objectundefinedfunctiontrue", "globals");
let outer = 5; function closes() { let inner = 6; return (() => outer + inner)(); }
check(closes(), 11, "closures");
function getters(o) { return o.g + o.h; }
check(getters({ get g() { return 1; }, __proto__: { get h() { return 2; } } }), 3, "getters");
function setters(o) { o.s = 5; return o.seen; }
check(setters({ set s(v) { this.seen = v; } }), 5, "setters");
function usesArguments() { return arguments.length + arguments[1]; }
check(usesArguments(1, 2, 3), 5, "arguments");
function spreads(a, ...rest) { return Math.max(...rest) + [...rest, a].length + { ...{ p: 1 }, q: 2 }.q; }
check(spreads(1, 2, 9), 14, "spread");
function destructures({ a, b: [c, d = 4] }, [e, ...f]) { return a + c + d + e + f.length; }
check(destructures({ a: 1, b: [2] }, [5, 6, 7]), 14, "destructuring");
class Private { #p = 1; static has(o) { return #p in o; } get() { return this.#p; } set(v) { this.#p = v; return this; } }
check(new Private().set(3).get() + Private.has(new Private) + Private.has({}), 4, "private names");
function evals(x) { return eval("x + 1"); }
check(evals(1), 2, "eval");
function tagged(x) { return ((s, v) => s.raw.join("|") + v)`a${x}b`; }
check(tagged(1), "a|b1", "tagged templates");
function switches(x) { switch (x) { case "a": return 1; case "b": return 2; default: return 3; } }
check(switches("b"), 2, "switch");
function* generates(n) { yield n; yield n + 1; return n + 2; }
check([...generates(1)].join(), "1,2", "generators");
async function awaits(x) { let y = await x; try { await Promise.reject(new Error("no")); } catch (e) { return y + e.message; } }
{ let r; awaits(1).then(v => { r = v; }); drainMicrotasks(); check(r, "1no", "async functions"); }

function throws(o) { return o.a.b; }
function catches(o) { try { return throws(o); } catch (e) { return e.constructor.name + ":" + (e.stack.split("\n")[0].startsWith("throws@") ? "throws" : e.stack.split("\n")[0]); } finally { finallyRan++; } }
let finallyRan = 0;
check(catches({}), "TypeError:throws", "an error, and where it was"); check(finallyRan, 1, "finally");
var firstLine = 49;
function line1() {
    return line2();
}
function line2() {
    return new Error("here").stack;
}
{
    let stack = line1().split("\n");
    check(stack[0].replace(/@.*:(\d+):\d+$/, "@$1"), "line2@" + (lineOf("line2") + 1), "the line in the innermost");
    check(stack[1].replace(/@.*:(\d+):\d+$/, "@$1"), "line1@" + (lineOf("line1") + 1), "the line in its caller");
}
function lineOf(name) { return name === "line1" ? firstLine : firstLine + 3; }
function thrower(depth) { if (!depth) throw new RangeError("deep"); return thrower(depth - 1) + 1; }
function catchesDeep() { try { thrower(20); } catch (e) { return e.message + e.stack.split("\n").filter(l => l.startsWith("thrower@")).length; } }
function overflows() { return overflows() + 1; }
function catchesOverflow() { try { overflows(); } catch (e) { return e instanceof RangeError; } }
function throwsInGetter(o) { return o.bad; }
function catchesFromGetter() { try { throwsInGetter({ get bad() { throw new SyntaxError("getter"); } }); } catch (e) { return e.message; } }
function nestedTry(x) { try { try { throw x; } finally { x += "f"; } } catch (e) { return e + x; } }

function recurses(o, depth) {
    let before = o.a + o.b;
    let below = depth ? recurses(o, depth - 1) : 0;
    o.count = (o.count || 0) + 1;
    return before + below + o.a + o.c.d + (o.missing === undefined ? 1 : 0) + { x: depth, y: 1 }.y;
}
function many(o) { return o.a + o.b + o.c.d; }
function polymorphic(o) { return o.k; }
function calledOften(i) { let o = { p: i, q: i + 1 }; o.r = o.p + o.q; return new Constructor(o.r, 1).sum() + Math.abs(-1); }
check(catchesDeep(), "deep21", "an error from deep down");
check(catchesOverflow(), true, "running out of stack");
check(catchesFromGetter(), "getter", "an error from a getter");
check(nestedTry("x"), "xxf", "try in try");
check(recurses({ a: 1, b: 2, c: { d: 3 } }, 200), 201 * (3 + 1 + 3 + 1 + 1), "recursion");
for (let round = 0; round < 3; ++round) {
    let total = 0;
    for (let i = 0; i < 300; ++i)
        total += many({ a: i, b: 1, c: { d: 2 } }) + calledOften(i);
    check(total, 300 * 3 + 44850 + (2 * 44850 + 300 + 300 + 300), "over and over");
    if (round == 1)
        gc();
}
for (let i = 0; i < 400; ++i)
    check(polymorphic(i % 4 == 0 ? { k: i } : i % 4 == 1 ? { j: 1, k: i } : i % 4 == 2 ? { i: 1, j: 2, k: i } : { __proto__: { k: i } }), i, "several shapes");
check(reads({ a: 1, b: 2, c: { d: 3 }, length: "!" }), "NaNtoString!", "reads, again");
check(JSON.stringify(stores({})), '{"a":3,"b":2,"c":4}', "stores, again");
check(catches({}), "TypeError:throws", "an error, again"); check(catches({ a: { b: 1 } }), 1, "no error");
check(constructs() + closes() + getters({ g: 1, h: 2 }), 17, "and the rest");
gc();
check(reads({ a: 1, b: 2, c: { d: 3 }, length: "!" }), "NaNtoString!", "after a collection");
check(recurses({ a: 1, b: 2, c: { d: 3 } }, 50), 51 * 9, "recursion, after a collection");
