//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1")
// Static scope resolution / scope caching must agree with JSScope::abstractResolve about which bindings a nested
// function sees. The validator turns any disagreement into a crash; the checks below cover the value semantics.

function shouldBe(actual, expected, what) {
    if (String(actual) !== String(expected))
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

// Closures created in a parameter list do not see the body's var/let/function declarations.
function paramClosure(a = () => { let before = typeof c; $.evalScript("var bytecodeOptimizerGlobalC = 42"); return before + "," + typeof bytecodeOptimizerGlobalC + "," + typeof c; }) {
    var c = 1;
    return a();
}
shouldBe(paramClosure(), "undefined,number,undefined", "parameter-scope closure vs body var");
function paramClosureLet(cb = () => typeof L) { let L = 1; class K { } return cb(); }
shouldBe(paramClosureLet(), "undefined", "parameter-scope closure vs body let");
function paramClosureFunction({ cb = () => typeof FD } = {}) { function FD() { } return cb(); }
shouldBe(paramClosureFunction(), "undefined", "parameter-scope closure vs hoisted function");

// A function's own name is only a binding for named function *expressions*.
eval("function evf() { return function() { let r1 = typeof evf; delete globalThis.evf; let r2 = typeof evf; return [r1, r2]; }; }");
shouldBe(evf()(), "function,undefined", "sloppy-eval function declaration is a deletable global");
var nf = new Function("return function() { return typeof anonymous; }");
shouldBe(nf()(), "undefined", "new Function is not bound as 'anonymous'");
function withAround(o) {
    with (o) {
        eval("function wf() { return () => { let r1 = typeof wf; o.wf = 'shadow'; let r2 = typeof wf; return [r1, r2]; }; }");
        return wf()();
    }
}
shouldBe(withAround({}), "function,string", "with object can shadow an eval-declared function later");
(function methods() {
    var o = {
        get gg() { return () => typeof gg; },
        ["ck"]() { return () => typeof ck; },
    };
    class K { static sm() { return () => typeof sm; } #pm() { return () => typeof pm; } callpm() { return this.#pm()(); } }
    shouldBe([o.gg(), o.ck()(), K.sm()(), new K().callpm()], "undefined,undefined,undefined,undefined", "method names are not in scope");
    const named = function self(n) { return n ? self(n - 1) + 1 : 0; };
    shouldBe(named(3), 3, "named function expression");
})();

// Class bodies are strict, but a sloppy eval in the function around them can still add vars they must see.
var shadowMe = "global";
function evalThenClass(code) {
    eval(code);
    class K { m() { return shadowMe; } static s() { return (() => shadowMe)(); } }
    return new K().m() + "/" + K.s();
}
shouldBe(evalThenClass("var shadowMe = 'evald'"), "evald/evald", "eval var visible from class method");
shouldBe(evalThenClass(""), "global/global", "no eval var");

// Depths: module-like nesting with per-iteration loop scopes, catch scopes, generators and async functions.
let top = "top";
function level1(p) {
    let l1 = "l1";
    function level2() {
        let l2 = "l2";
        const out = [];
        for (let i = 0; i < 2; i++) {
            let perIteration = i * 10;
            out.push((() => [top, l1, l2, perIteration, i].join("/"))());
        }
        try { throw new Error("caught"); } catch (err) { out.push((() => err.message + "/" + l1 + "/" + top)()); }
        function* g() { yield top; yield l2; }
        out.push([...g()].join("/"));
        class C { static s = top + "/" + l2; m() { return [top, l1, C.s].join("/"); } }
        out.push(new C().m());
        return [out, async () => { await null; return [top, l1, l2, p].join("/"); }];
    }
    return level2();
}
const [out, later] = level1("p");
shouldBe(out.join("|"), "top/l1/l2/0/0|top/l1/l2/10/1|caught/l1/top|top/l2|top/l1/top/l2", "static depths");
top = "TOP";
let asyncFailure;
later().then(v => shouldBe(v, "TOP/l1/l2/p", "closure sees reassigned outer let")).catch(e => { asyncFailure = e; });
drainMicrotasks();
if (asyncFailure)
    throw asyncFailure;
