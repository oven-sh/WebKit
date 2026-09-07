//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1")
//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Static scope resolution / scope caching / TDZ-check elimination must not change which binding a nested function
// sees. The validator turns any disagreement with JSScope::abstractResolve into a crash; the expectations below pin the
// value semantics (recorded from an unoptimized run). Each section was an attack on the DeclaredNamesLink model:
// catch parameter scopes incl. destructuring and shadowing
// per-iteration loop environments with closures; scope cache across iterations
// switch-block lexical scope, labels/break/continue through nested scopes, finally
// closures created in finally blocks reached by return/break/throw, iterator close, `using`
// closures created in loop heads (destructuring defaults, test/update expressions)
// scope-cache dataflow across block push/pop, try/catch/finally, with, switch, generators
function shouldBe(actual, expected, what) {
    if (String(actual) !== String(expected))
        throw new Error(what + ": expected " + JSON.stringify(String(expected)) + " but got " + JSON.stringify(String(actual)));
}
let __expected, __idx, __what;
function section(what, expected) { if (__expected && __idx !== __expected.length) throw new Error(__what + ": only " + __idx + " of " + __expected.length + " checks ran"); __what = what; __expected = expected; __idx = 0; }
function print(...args) { const v = args.map(String).join(" "); for (const line of v.split("\n")) shouldBe(line, __expected[__idx++], __what + " #" + __idx); }

// --- catch parameter scopes incl. destructuring and shadowing
section("04-catch.js", ["A.B.ox|A.c.ox|dox|efox|shadow|ox"]);
{
function outer() {
  let x = "ox"; const out = [];
  try { throw {a: "A", b: ["B"]}; } catch ({a, b: [b]}) { out.push((() => [a, b, x].join("."))()); let c = "c"; out.push((() => () => [a, c, x].join("."))()()); }
  try { throw 1; } catch { let d = "d"; out.push((() => d + x)()); }
  try { throw "e"; } catch (e) { try { throw "f"; } catch (f) { out.push((function() { return () => e + f + x; })()()); } }
  // catch param shadowing outer
  try { throw "shadow"; } catch (x) { out.push((() => x)()); }
  out.push((() => x)());
  return out.join("|");
}
print(outer());

}

// --- per-iteration loop environments with closures; scope cache across iterations
section("05-loops.js", ["ox:0:0:0|ox:2:4:0|ox:0:0:0|ox:1:2:0|ox:2:4:0|7-0ox|7-1ox|8-0ox|8-1ox|aox|box|0ox|1ox|doox", "A0AA1AA2A"]);
{
function outer() {
  let x = "ox"; const fs = []; const out = [];
  for (let i = 0, f0 = () => i; i < 3; i++) { let j = i * 2; fs.push(() => [x, i, j, f0()].join(":")); if (i == 1) continue; out.push(fs[fs.length-1]()); }
  for (const k of [7, 8]) { L: for (let m = 0; m < 2; m++) { fs.push(() => k + "-" + m + x); if (m) break L; } }
  for (let p in {a:1, b:2}) fs.push(() => p + x);
  let w = 0; while (w < 2) { let wv = w++; fs.push(() => wv + x); }
  do { let dv = "do"; fs.push(() => dv + x); } while (false);
  return out.concat(fs.map(f => f())).join("|");
}
print(outer());
// closure in loop referencing outer twice (cache) across per-iteration scope recreation
function cache() {
  let a = "A"; let r = "";
  for (let i = 0; i < 3; i++) { let q = i; const g = () => q; r += a + g() + a; }
  return r;
}
print(cache());

}

// --- switch-block lexical scope, labels/break/continue through nested scopes, finally
section("12-switch-misc.js", ["ox,a0 ox,a1,b oxReferenceError", "x000|x103|x0|f0x|x1|f1x"]);
{
function outer(n) {
  let x = "ox";
  switch (n) { case 0: case 1: let a = "a" + n; if (n == 0) return () => [x, a].join(); { const b = "b"; return () => () => [x, a, b].join(); } default: { class Q { static v = (() => { try { return a; } catch (e) { return x + e.constructor.name; } })(); } return () => Q.v; } }
}
print(outer(0)(), outer(1)()(), outer(2)());
// labels/break/continue with closures inside nested lexical scopes
function flow() {
  let x = "x"; const fs = [];
  outerLoop: for (let i = 0; i < 3; i++) { inner: for (let j = 0; j < 3; j++) { let k = i * 3 + j; if (j == 1) continue outerLoop; if (k == 6) break outerLoop; fs.push(() => [x, i, j, k].join("")); } }
  try { for (let t = 0; t < 2; t++) { try { let u = t; fs.push(() => x + u); if (t) throw 0; } finally { let fin = "f" + t; fs.push(() => fin + x); } } } catch {}
  return fs.map(f => f()).join("|");
}
print(flow());

}

// --- closures created in finally blocks reached by return/break/throw, iterator close, `using`
section("14-finally-closures.js", ["bcox|b,f,ox|d1lox|d1,l,1,ox|d2,l,2,ox|lox|q,v,w,ox|closedox|au0|u0oxobject"]);
{
function outer() {
  let x = "ox"; const fs = [];
  function ret() { { let b = "b"; try { { let c = "c"; fs.push(() => b + c + x); return "r"; } } finally { let f = "f"; fs.push(() => [b, f, x].join(",")); } } }
  ret();
  lab: { let l = "l"; try { for (let i of [1, 2]) { let d = "d" + i; try { if (i == 2) break lab; throw 0; } catch (e) { fs.push(() => d + l + x); continue; } finally { fs.push(() => [d, l, i, x].join(",")); } } } finally { fs.push(() => l + x); } }
  // iterator close via return inside for-of inside block scopes
  const it = { [Symbol.iterator]() { return { next: () => ({ value: "v", done: false }), return: () => { fs.push(() => "closed" + x); return {}; } }; } };
  (function() { let q = "q"; for (const v of it) { let w = "w"; fs.push(() => [q, v, w, x].join(",")); return; } })();
  // using declarations if supported
  try { eval('(function() { let u0 = "u0"; { using u = { [Symbol.dispose]() { fs.push(() => u0 + x + typeof u); } }; let after = "a"; fs.push(() => after + u0); } })()'); } catch (e) { fs.push(() => "no-using"); }
  return fs.map(f => f()).join("|");
}
print(outer());

}

// --- closures created in loop heads (destructuring defaults, test/update expressions)
section("15-loophead-closures.js", ["() => [a, b, x].join(\",\"),1,ox|() => [a, b, x].join(\",\"),2,ox|string|string|0ox|0|1ox|1|2ox|1,0,1,ox|2,0,2,ox"]);
{
function outer() {
  let x = "ox"; const fs = [];
  for (let {a = () => [a, b, x].join(","), b} of [{b: 1}, {b: 2}]) fs.push(a);
  for (let [c = () => c + x] in {k1: 1, k2: 2}) fs.push(() => typeof c);
  let f; for (let i = 0; (f = () => i + x, i < 2); i++) { fs.push(f); fs.push(() => i); }
  fs.push(f);
  for (let i = 0, g = () => i; i < 2; i++, fs.push(((j) => () => [j, g(), i, x].join(","))(i))) {}
  return fs.map(f => f()).join("|");
}
print(outer());

}

// --- scope-cache dataflow across block push/pop, try/catch/finally, with, switch, generators
section("20-cache-flow.js", ["aba0aababa0aa1aababababb1aa2aabab2aa3aababababb3a[a]abaa", "aba0aababa0aa1aababababb1aa2aabab2a[W]abWa", "aa,a,aaa0aa,a,aaa0aaa1aa"]);
{
function mk() {
  let a = "a", b = "b";
  return function f(n, o) {
    let r = a + b;                       // depth 0: mov from scope
    for (let i = 0; i < n; i++) {        // i captured below -> per-iteration env, scopeReg redefined
      r += a;                            // depth 1
      { let c = i; r += (() => c + a)(); r += a + b; }   // depth 2 then back
      if (i & 1) { try { r += a; throw b; } catch (e) { r += e + a; let d = e; (() => d); } finally { r += b; } }
      label: { r += a; if (i == 2) break label; r += b; }
      switch (i) { case 0: let s = a; r += s; (() => s); break; default: r += b; }
      r += (() => i)() + a;
    }
    with (o) { r += "[" + a + "]"; }     // dynamic inside with; after with, static again
    r += a + b;
    try { with (o) { throw a; } } catch (e) { r += e; }
    return r + a;
  };
}
print(mk()(4, {}));
print(mk()(3, {a: "W"}));
// generator flavour
function mkg() {
  let a = "a";
  return function* g(n) { let r = a; for (let i = 0; i < n; i++) { r += a; yield r; r += a + (() => i)(); { let q = a; yield () => q; r += q; } } return r + a; };
}
const it = mkg()(2); let acc = []; for (let s = it.next(); ; s = it.next()) { acc.push(typeof s.value == "function" ? s.value() : s.value); if (s.done) break; }
print(acc.join(","));

}

section("end", []);
