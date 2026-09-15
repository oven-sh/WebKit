//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1")
//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Static scope resolution / scope caching / TDZ-check elimination must not change which binding a nested function
// sees. The validator turns any disagreement with JSScope::abstractResolve into a crash; the expectations below pin the
// value semantics (recorded from an unoptimized run). Each section was an attack on the DeclaredNamesLink model:
// parameter scope vs body scope (defaults, destructuring, rest, arguments, duplicates)
// arrow functions capturing this/new.target/arguments/super incl. derived constructors
// generator / async / async-generator wrapper+body scopes, class async arrow fields; cache reset at yield
// sloppy mapped arguments (ScopedArguments) with captured/duplicate/function-shadowed parameters
// `arguments` seen from closures inside generator/async bodies (resolved in the wrapper)
// catch destructuring defaults, param TDZ, contextual-keyword names, static blocks, for-await labels, super in object methods, tagged templates, using, closures in super() args
function shouldBe(actual, expected, what) {
    if (String(actual) !== String(expected))
        throw new Error(what + ": expected " + JSON.stringify(String(expected)) + " but got " + JSON.stringify(String(actual)));
}
let __expected, __idx, __what;
function section(what, expected) { if (__expected && __idx !== __expected.length) throw new Error(__what + ": only " + __idx + " of " + __expected.length + " checks ran"); __what = what; __expected = expected; __idx = 0; }
function print(...args) { const v = args.map(String).join(" "); for (const line of v.split("\n")) shouldBe(line, __expected[__idx++], __what + " #" + __idx); }

// --- parameter scope vs body scope (defaults, destructuring, rest, arguments, duplicates)
section("06-params.js", ["ox/oxundefined/ox,undefined/2ox|2ox|undefinedox|oxobject|5ox"]);
{
function outer() {
  let x = "ox";
  function f(a = () => x, b = () => a() + typeof y, {c = () => [x, typeof d].join()} = {}, ...rest) { var y = "y"; let d = 1; return [a(), b(), c(), (() => rest.length + x)()].join("/"); }
  function g(a, a2 = arguments, h = () => a2.length + arguments.length + x) { return h(); }
  function dup(p, p) { return (() => typeof p + x)(); } // sloppy duplicate params
  const arrow = (a = () => x + typeof arguments) => a();
  function argsShadow(arguments = 5, k = () => arguments) { return k() + x; }
  return [f(undefined, undefined, undefined, 1, 2), g(1), dup(1), arrow(), argsShadow()].join("|");
}
print(outer());

}

// --- arrow functions capturing this/new.target/arguments/super incl. derived constructors
section("07-arrow-this.js", ["v,true,ox,arg|bmoxox"]);
{
function outer() {
  let x = "ox";
  function F() { this.v = "v"; const a = () => () => [this.v, new.target === F, x, arguments[0]].join(","); this.r = a()(); this.toString = () => this.r; }
  class B { m() { return "bm"; } }
  class D extends B { m() { const a = () => (() => super.m() + x)(); return a(); } constructor() { const pre = () => x; pre(); super(); this.k = (() => this.m() + x)(); } }
  return [new F("arg"), new D().k].join("|");
}
print(outer());

}

// --- generator / async / async-generator wrapper+body scopes, class async arrow fields; cache reset at yield
section("08-generators.js", ["ox,P,l|ox,P,l,b|ox1|oxM|ox,A,al|ox,AA,q,false|oxC|oxG|oxG", "aaa1undefined"]);
{
function outer() {
  let x = "ox";
  function* g(p) { let l = "l"; yield () => [x, p, l].join(","); { let b = "b"; yield () => [x, p, l, b].join(","); } for (let i of [1]) yield () => x + i; }
  async function af(p) { let l = "al"; await null; return (() => () => [x, p, l].join(","))()(); }
  const aa = async (p) => { await null; let q = "q"; return (() => [x, p, q, this === undefined].join(","))(); };
  async function* ag(p) { yield (() => x + p)(); await null; yield (function() { return () => x + p; })()(); }
  class C { f = async () => { await null; return (() => x + this.constructor.name)(); }; *gm(p) { yield () => x + p; } }
  const out = [];
  for (const f of g("P")) out.push(f());
  for (const f of new C().gm("M")) out.push(f());
  af("A").then(v => out.push(v)); aa("AA").then(v => out.push(v)); new C().f().then(v => out.push(v));
  (async () => { for await (const v of ag("G")) out.push(v); })();
  drainMicrotasks();
  return out.join("|");
}
print(outer());
// generator: resolve outer twice across a yield in same activation (cache reset at yield)
function genCache() { let a = "a"; function* g() { let r = a; yield 0; r += a; { let z = 1; yield () => z; r += a + z; } return r; } const it = g(); it.next(); it.next(); return it.next().value + it.next().value; }
print(genCache());

}

// --- sloppy mapped arguments (ScopedArguments) with captured/duplicate/function-shadowed parameters
section("13-scopedargs.js", ["2,1,2,ox|function,B!,ox|changedox|origox|origchangedox"]);
{
function outer() {
  let x = "ox";
  function f(a, a) { arguments; return (() => [a, arguments[0], arguments[1], x].join(","))(); }
  function g(a, b) { function a() {} arguments[1] = "B!"; return (() => [typeof a, b, x].join(","))(); }
  function h(a) { var a; arguments[0] = "changed"; return (() => a + x)(); }
  function k(a) { "use strict"; arguments[0] = "changed"; return (() => a + x)(); }
  function m(a, b = () => a) { arguments[0] = "changed"; return b() + (() => arguments[0] + x)(); }
  return [f(1, 2), g(1, 2), h("orig"), k("orig"), m("orig")].join("|");
}
print(outer());

}

// --- `arguments` seen from closures inside generator/async bodies (resolved in the wrapper)
section("17-gen-arguments.js", ["2|A,A,v,ox|objectA|3,B,w,ox|1,C,w2,ox|object,D,2,ox|E,ox"]);
{
function outer() {
  let x = "ox";
  function* g(a) { var v = "v"; yield arguments.length; yield () => [arguments[0], a, v, x].join(","); yield (function() { return () => typeof arguments + a; })()(); }
  async function af(a) { var w = "w"; return () => [arguments.length, a, w, x].join(","); } // no await: inlined body
  async function af2(a) { await 0; var w = "w2"; return () => [arguments.length, a, w, x].join(","); }
  const aa = async (a, ...r) => { await 0; return () => [typeof arguments, a, r.length, x].join(","); };
  function wrapArgs() { const aa2 = async () => { await 0; return () => [arguments[0], x].join(","); }; return aa2; }
  const out = []; const it = g("A", 2); out.push(it.next().value); out.push(it.next().value()); out.push(it.next().value);
  af("B", 1, 2).then(f => out.push(f())); af2("C").then(f => out.push(f())); aa("D", 1, 2).then(f => out.push(f())); wrapArgs("E")().then(f => out.push(f()));
  drainMicrotasks();
  return out.join("|");
}
print(outer());

}

// --- catch destructuring defaults, param TDZ, contextual-keyword names, static blocks, for-await labels, super in object methods, tagged templates, using, closures in super() args
section("21-exotic.js", ["function:x", "function:xz", "B,BB", "aw,L,undefined", "sv,sl,x,function", "x5,1,0,10", "x5,1,1,11", "x5,2,0,20", "x5", "pmx6", "a|b|1x6x6", "wx7", "disposed:x7", "pre,x8,no-this", "pre,object"]);
{
var out = [];
function t1() { let x = "x"; try { throw {}; } catch ({ a = () => [typeof a, x].join(":") }) { var a2 = a; let z = "z"; out.push(a(), (() => a2() + z)()); } }
t1();
function t2(a = () => { try { return b; } catch (e) { return "tdz"; } }, b = "B") { const early = a; return [early(), (() => a() + b)()].join(","); }
out.push(t2());
function t3() { let let_ = "L"; var yield_; { let await = "aw"; function arguments() { return () => [await, let_, typeof yield_].join(","); } out.push(arguments()()); } }
t3();
function t4() { let x = "x"; class C { static { var sv = "sv"; let sl = "sl"; C.f = () => () => [sv, sl, x, typeof C].join(","); } } out.push(C.f()()); }
t4();
async function* t5() { let x = "x5"; outer: for await (const v of [1, 2, 3]) { for (let i = 0; i < 2; i++) { let k = v * 10 + i; if (k == 21) continue outer; if (k == 30) break outer; yield () => [x, v, i, k].join(","); } } yield () => x; }
(async () => { for await (const f of t5()) out.push(f()); })();
drainMicrotasks();
function t6() { let x = "x6"; const o = { m() { return () => super.pm() + x; }, __proto__: { pm() { return "pm"; } } }; out.push(o.m()()); const tag = (s, ...v) => () => s.raw.join("|") + v.join("") + x; out.push(tag`a${1}b${(() => x)()}`()); }
t6();
function t7() { let x = "x7"; try { eval('{ using u = { [Symbol.dispose]() { out.push((() => "disposed:" + x)()); } }; await0: { let w = "w"; out.push((() => w + x)()); } }'); } catch (e) { out.push("nousing"); } }
t7();
// closures created inside argument lists of super() in derived ctor before this is available
function t8() { let x = "x8"; class B { constructor(f) { this.r = f(); } } class D extends B { constructor() { let pre = "pre"; super(() => [pre, x, (() => { try { this; return "this?"; } catch { return "no-this"; } })()].join(",")); out.push(this.r, (() => [pre, typeof this].join(","))()); } } new D(); }
t8();
print(out.join("\n"));

}

section("end", []);
