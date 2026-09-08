//@ runDefault
//@ runDefault("--useGlobalResolveMemo=0")
//@ runDefault("--useBatchedLazyLink=0")
//@ runDefault("--useLazyCodeBlockLink=0")
//@ runDefault("--useConcurrentJIT=0", "--jitPolicyScale=0")
// JSScope::abstractResolve's global resolve memo must be invalidated whenever what it memoized at the global lexical
// environment / global object changes, and CodeBlock::linkAllLazily must link every scope op of a block against the
// right scope. Each section links a fresh CodeBlock (new Function / $.evalScript) after the memo was populated.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + String(expected) + " but got " + String(actual));
}
noInline(shouldBe);

function shouldThrow(func, errorType, what) {
    let error = null;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(what + ": expected " + errorType.name + " but got " + String(error));
}
noInline(shouldThrow);

// 1. GlobalProperty, then a global lexical binding shadows it (epoch bump).
globalThis.gp = 1;
function readGP() { return gp; }
noInline(readGP);
for (let i = 0; i < 100; ++i)
    shouldBe(readGP(), 1, "gp before shadowing");
$.evalScript("let gp = 2;");
shouldBe(new Function("return gp;")(), 2, "new block after let gp");
shouldBe(readGP(), 2, "old block after let gp");
shouldBe(globalThis.gp, 1, "property untouched");

// 2. UnresolvedProperty (typeof probe), then a global var is declared (symbol table entry).
function probeUV() { return typeof uv; }
noInline(probeUV);
shouldBe(probeUV(), "undefined", "uv probe");
shouldThrow(new Function("return uv;"), ReferenceError, "uv unresolved");
$.evalScript("var uv = 3;");
shouldBe(new Function("return uv;")(), 3, "new block after var uv");
shouldBe(probeUV(), "number", "old block after var uv");
new Function("uv = 4;")();
shouldBe(uv, 4, "put to global var");

// 3. GlobalProperty deleted, then re-added.
globalThis.dp = 1;
function readDP() { return dp; }
noInline(readDP);
shouldBe(readDP(), 1, "dp");
shouldBe(delete globalThis.dp, true, "delete dp");
shouldThrow(new Function("return dp;"), ReferenceError, "dp after delete (new block)");
shouldThrow(readDP, ReferenceError, "dp after delete (old block)");
globalThis.dp = 5;
shouldBe(new Function("return dp;")(), 5, "dp re-added (new block)");
shouldBe(readDP(), 5, "dp re-added (old block)");

// 4. Put to a GlobalProperty whose replacement watchpoint is intact, then made read-only.
globalThis.pp = 1;
function writePP(v) { pp = v; }
noInline(writePP);
writePP(2);
shouldBe(pp, 2, "pp put 1");
new Function("pp = 3;")();
shouldBe(pp, 3, "pp put 2");
for (let i = 0; i < 100; ++i)
    writePP(i);
Object.defineProperty(globalThis, "pp", { value: 4, writable: false, configurable: true });
new Function("pp = 9;")();
shouldBe(pp, 4, "sloppy put to read-only pp (new block)");
writePP(9);
shouldBe(pp, 4, "sloppy put to read-only pp (old block)");
shouldThrow(new Function("'use strict'; pp = 9;"), TypeError, "strict put to read-only pp");

// 5. const global lexical binding: puts must keep throwing (Dynamic), reads see the binding.
$.evalScript("const cc = 7;");
shouldBe(new Function("return cc;")(), 7, "read cc");
shouldThrow(new Function("cc = 8;"), TypeError, "put cc 1");
shouldThrow(new Function("cc = 8;"), TypeError, "put cc 2");
shouldBe(cc, 7, "cc unchanged");

// 6. Var injection: sloppy direct eval below the global levels changes the answer per chain, not per name.
var vi = 10;
function injected() { eval("var vi = 20;"); return (function () { return vi; })(); }
noInline(injected);
function notInjected() { return (function () { return vi; })(); }
noInline(notInjected);
for (let i = 0; i < 50; ++i) {
    shouldBe(injected(), 20, "injected vi");
    shouldBe(notInjected(), 10, "global vi");
}

// 7. Batched link from various frame kinds: generators, async functions, class fields, arrow functions, with.
globalThis.fk = 100;
function* gen() { let a = fk; yield a; yield fk + uv; }
let g = gen();
shouldBe(g.next().value, 100, "generator 1");
shouldBe(g.next().value, 104, "generator 2");
class K { x = fk; static y = fk + 1; #z = fk + 2; z() { return this.#z; } }
shouldBe(K.y, 101, "static field");
shouldBe(new K().x, 100, "instance field");
shouldBe(new K().z(), 102, "private field");
let arrow = () => { let o = { fk: 1 }; with (o) { return fk; } };
shouldBe(arrow(), 1, "with shadows global");
let asyncResult = null;
(async function () { await null; asyncResult = fk + cc; })();
drainMicrotasks();
shouldBe(asyncResult, 107, "async function");

// 8. A function whose first scope op to execute is not its first scope op in bytecode order.
function branchy(b) {
    if (b)
        return dp + pp;
    let local = 0;
    let inc = () => { local += fk; };
    inc();
    return local + gp;
}
noInline(branchy);
shouldBe(branchy(false), 102, "branchy false");
shouldBe(branchy(true), 9, "branchy true");
for (let i = 0; i < 100; ++i)
    branchy(i & 1);

// 9. New global function declaration from a later script after an UnresolvedProperty probe.
shouldBe(new Function("return typeof laterFunction;")(), "undefined", "laterFunction probe");
$.evalScript("function laterFunction() { return 42; }");
shouldBe(new Function("return laterFunction();")(), 42, "laterFunction call");
