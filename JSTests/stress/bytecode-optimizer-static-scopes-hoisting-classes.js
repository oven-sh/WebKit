//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1")
//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Static scope resolution / scope caching / TDZ-check elimination must not change which binding a nested function
// sees. The validator turns any disagreement with JSScope::abstractResolve into a crash; the expectations below pin the
// value semantics (recorded from an unoptimized run). Each section was an attack on the DeclaredNamesLink model:
// Annex B block-level function hoisting (sloppy), switch/label blocks
// named function expression name scope (strict/sloppy, captured or not)
// class head/body scopes: name binding, static/instance field initializers, computed keys, static blocks, extends expressions, private names
// `with` around hoisted / block functions and closures created before/inside/after it
// private names and brand checks resolved through enclosing class scopes, shadowed by inner classes
function shouldBe(actual, expected, what) {
    if (String(actual) !== String(expected))
        throw new Error(what + ": expected " + JSON.stringify(String(expected)) + " but got " + JSON.stringify(String(actual)));
}
let __expected, __idx, __what;
function section(what, expected) { if (__expected && __idx !== __expected.length) throw new Error(__what + ": only " + __idx + " of " + __expected.length + " checks ran"); __what = what; __expected = expected; __idx = 0; }
function print(...args) { const v = args.map(String).join(" "); for (const line of v.split("\n")) shouldBe(line, __expected[__idx++], __what + " #" + __idx); }

// --- Annex B block-level function hoisting (sloppy), switch/label blocks
section("01-hoist-annexb.js", ["block-x,block-x,outer-x,outer-x/sw,outer-x/sw", "1"]);
{
// Annex B function-in-block, sloppy: inner function created at block entry; also var-hoisted alias.
var r = [];
function outer() {
  let x = "outer-x";
  {
    let x = "block-x";
    function inner() { return x; }
    r.push(inner());
  }
  r.push(inner()); // var-scoped alias, same function object, still block-x
  if (true) { function inner2() { return x; } }
  r.push(inner2());
  switch (1) { case 1: let x2 = "sw"; function inner3() { return x + "/" + x2; } r.push(inner3()); }
  r.push(inner3());
  return r.join(",");
}
print(outer());
// label + block function
function lab() { let q = 1; L: { function ff() { return q; } break L; } return ff(); }
print(lab());

}

// --- named function expression name scope (strict/sloppy, captured or not)
section("02-named-funcexpr.js", ["oy0function|function:o", "function"]);
{
function outer() {
  let x = "o";
  var f = function self(n) { let y = "y" + n; return n ? self(n-1) : (() => x + y + typeof self)(); };
  var g = function self2() { return () => () => [typeof self2, x].join(":"); };
  return f(2) + "|" + g()()();
}
print(outer());
// sloppy: assignment to name is ignored; name scope captured
var h = function nm() { nm = 5; return (() => typeof nm)(); };
print(h());

}

// --- class head/body scopes: name binding, static/instance field initializers, computed keys, static blocks, extends expressions, private names
section("03-class-scopes.js", ["function,ox|function,ox,K|ox,function,oxK|ox,function|ox,true|oxfunction|ox", "function1", "z"]);
{
function outer() {
  let x = "ox"; let K = "outerK";
  class K2 { static f() { return () => [typeof K2, x].join(","); } }
  const E = class K {
    static s = (() => [typeof K, x, K.name].join(","))();
    #p = (() => x + K.name)();
    [(() => x + "key")()]() { return () => [x, typeof K, this.#p].join(","); }
    static { this.blk = (() => [x, typeof K].join(","))(); }
    m() { return (() => () => [x, K === E].join(","))()(); }
  };
  const D = class extends ((() => { return class B { bx() { return () => x; } }; })()) {
    constructor() { super(); this.v = (() => x + typeof D)(); }
  };
  return [K2.f()(), E.s, new E()["oxkey"]()(), E.blk, new E().m(), new D().v, new D().bx()()].join("|");
}
print(outer());
// anonymous class: no name binding
function anon() { let y = 1; const C = class { m() { return () => typeof C + y; } }; return new C().m()(); }
print(anon());
// class in class computed key
function nested() { let z = "z"; class A { static [ (class B { static k = (() => z)(); }).k ] () { return () => z; } } return A.z()(); }
print(nested());

}

// --- `with` around hoisted / block functions and closures created before/inside/after it
section("16-with-hoist.js", ["fx,wx|fx,i,1|wx,i,1|fx2|globalwith,l,m|globalwith,l,m"]);
{
var out = [];
function f(o) { let x = "fx"; if (true) function g() { return () => x; } with (o) { function h() { return () => x; } } return [g()(), h()()]; }
out.push(f({x: "wx"}));
function f2(o) { let x = "fx"; with (o) with ({}) { { let inner = "i"; var fe = function() { let y = 1; return () => [x, inner, y].join(","); }; } } return fe()(); }
out.push(f2({}), f2({x: "wx"}));
// closure created before with in same function, invoked inside; and function whose enclosing has with but not on path
function f3(o) { let x = "fx"; const pre = function() { let z = 2; return () => x + z; }; with (o) { return pre()(); } }
out.push(f3({x: "no"}));
// global with around everything
with ({x: "globalwith"}) { var f4 = function() { let l = "l"; return function() { let m = "m"; return () => [x, l, m].join(","); }; }; }
out.push(f4()()());
var x = "gx";
out.push(f4()()());
print(out.join("|"));

}

// --- private names and brand checks resolved through enclosing class scopes, shadowed by inner classes
section("19-private.js", ["true,Ap,Am,Ag,Asp,Asm,ox|false,Ap,Am,Ag,Asp,Asm,ox|false,false,Bp,Am,ox/true,false,Bp,Am,ox|Asp,y,ox", "1,true,2"]);
{
function outer() {
  let x = "ox";
  class A {
    #p = "Ap"; static #sp = "Asp"; #m() { return "Am"; } get #g() { return "Ag"; } static #sm() { return "Asm"; }
    probe(o) { return (() => [#p in o, this.#p, this.#m(), this.#g, A.#sp, A.#sm(), x].join(","))(); }
    inner() {
      const self = this;
      class B { #p = "Bp"; probe(o) { return (() => () => [#p in o, #p in self, this.#p, self.#m(), x].join(","))()(); } }
      return new B().probe(this) + "/" + new B().probe(new B());
    }
    static nested() { return (function() { let y = "y"; return () => [A.#sp, y, x].join(","); })()(); }
  }
  const a = new A();
  return [a.probe(a), a.probe({}), a.inner(), A.nested()].join("|");
}
print(outer());
// brand checks across generator/async boundaries
function outer2() {
  class C { #v = 1; *g() { yield () => this.#v; let o = this; yield (function() { return () => #v in o; })()(); } async a() { await 0; return (() => this.#v + 1)(); } }
  const out = []; for (const f of new C().g()) out.push(typeof f == "function" ? f() : f);
  new C().a().then(v => out.push(v)); drainMicrotasks();
  return out.join(",");
}
print(outer2());

}

section("end", []);
