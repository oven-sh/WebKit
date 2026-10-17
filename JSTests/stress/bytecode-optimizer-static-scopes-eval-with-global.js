//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1")
//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Static scope resolution / scope caching / TDZ-check elimination must not change which binding a nested function
// sees. The validator turns any disagreement with JSScope::abstractResolve into a crash; the expectations below pin the
// value semantics (recorded from an unoptimized run). Each section was an attack on the DeclaredNamesLink model:
// `with` at various depths, Symbol.unscopables, Proxy scope objects
// direct/indirect eval (sloppy/strict) in the function, in enclosing functions, in arrows/params/methods; new Function; delete of eval vars
// global property later shadowed by a global lexical binding ($.evalScript); second script
function shouldBe(actual, expected, what) {
    if (String(actual) !== String(expected))
        throw new Error(what + ": expected " + JSON.stringify(String(expected)) + " but got " + JSON.stringify(String(actual)));
}
let __expected, __idx, __what;
function section(what, expected) { if (__expected && __idx !== __expected.length) throw new Error(__what + ": only " + __idx + " of " + __expected.length + " checks ran"); __what = what; __expected = expected; __idx = 0; }
function print(...args) { const v = args.map(String).join(" "); for (const line of v.split("\n")) shouldBe(line, __expected[__idx++], __what + " #" + __idx); }

// --- `with` at various depths, Symbol.unscopables, Proxy scope objects
section("09-with.js", ["wx|wxy|ox|ox,m|ox,b,undefined|ox,b,undefined|late,b,number|ox|ox|pxpx|2"]);
{
function outer() {
  let x = "ox"; const out = [];
  with ({x: "wx"}) { out.push((() => x)()); out.push((function() { let y = "y"; return () => x + y; })()()); }
  out.push((() => x)());
  function mid() { let m = "m"; with ({}) { return () => [x, m].join(","); } }
  out.push(mid()());
  function mid2(o) { with (o) { { let b = "b"; return () => () => [x, b, typeof m2].join(","); } } }
  const f2 = mid2({}); out.push(f2()()); 
  const o3 = {}; const f3 = mid2(o3); out.push(f3()()); o3.x = "late"; o3.m2 = 1; out.push(f3()());
  // with deeper than closure creation: closure created outside with but called inside
  const pre = () => x; with ({x: "no"}) out.push(pre());
  // Symbol.unscopables
  const u = {x: "ux", [Symbol.unscopables]: {x: true}}; with (u) out.push((() => x)());
  // Proxy as with object
  let log = []; const p = new Proxy({}, {has(t, k) { log.push(String(k)); return k === "x"; }, get(t, k) { return k === Symbol.unscopables ? undefined : "px"; }});
  with (p) { out.push((() => x + x)()); }
  out.push(log.filter(k => k === "x").length);
  return out.join("|");
}
print(outer());

}

// --- direct/indirect eval (sloppy/strict) in the function, in enclosing functions, in arrows/params/methods; new Function; delete of eval vars
section("10-eval.js", ["ox,n|ox,number|evx,undefined|ox,undefined|ox,s,undefined|ax,number|ox,undefined|stringundefined|ox,e|gx,ie|gx,nf|ox,undefined|string,ox", "undefined,1,gx/number,1,lx"]);
{
var x = "gx";
function outer(code) {
  let x = "ox"; let r = [];
  function noEval() { let n = "n"; return () => [x, n].join(","); }
  r.push(noEval()());
  function withEval(c) { eval(c); return () => (() => [x, typeof ev].join(","))(); }
  r.push(withEval("var ev = 1")()); r.push(withEval("var x = 'evx'")()); r.push(withEval("")());
  function strictEval(c) { "use strict"; eval(c); let s = "s"; return () => [x, s, typeof ev].join(","); }
  r.push(strictEval("var ev = 1; var x = 'no'")());
  const arrowEval = (c) => { eval(c); return () => [x, typeof av].join(","); };
  r.push(arrowEval("var av = 1; var x = 'ax'")()); r.push(arrowEval("")());
  function delEval() { eval("var dv = 'dv'"); const f = () => typeof dv; const a = f(); eval("delete dv"); return a + f(); }
  r.push(delEval());
  // eval creating closures
  r.push(eval("(function(){ let e = 'e'; return () => [x, e].join(','); })")()());
  r.push((0, eval)("(function(){ let e = 'ie'; return () => [x, e].join(','); })")()());
  r.push(new Function("let nf = 'nf'; return () => [x, nf].join(',');")()());
  // eval inside class method / arrow param
  class C { m(c) { eval(c); return () => [x, typeof cm].join(","); } }
  r.push(new C().m("var cm = 1")());
  function paramEval(a = eval("var pe = 'pe'"), f = () => [typeof pe, x].join(",")) { var pe2; return f(); }
  r.push(paramEval());
  return r.join("|");
}
print(outer());
// eval in enclosing function adds var AFTER closure created; closure in inner non-eval function
function late(code) { function mid() { let m = 1; return () => [typeof lateVar, m, x].join(","); } const f = mid(); const a = f(); eval(code); return a + "/" + f(); }
print(late("var lateVar = 2, x = 'lx'"));

}

// --- global property later shadowed by a global lexical binding ($.evalScript); second script
section("11-global-shadow.js", ["global-prop,l,undefined", "global-let,l,number", "global-let,l,number", "global-let,l,number", "global-let,l2"]);
{
globalThis.gp = "global-prop"; // configurable global property: can be shadowed by a later global let
function outer() { let l = "l"; function mid() { return () => [gp, l, typeof laterLet].join(","); } return mid(); }
const f = outer();
print(f());
$.evalScript("let gp = 'global-let'; let laterLet = 1;");
print(f());
print(outer()());
globalThis.l = "not me";
print(f());
// second script defines function using same names
$.evalScript("function outer2() { let l = 'l2'; return () => [gp, l].join(','); } print(outer2()());");

}

section("end", []);
