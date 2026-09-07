//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1")
// Redundant TDZ-check elimination must not treat a name that a `with` object or a sloppy eval var can answer as one
// binding: the first read passes on the shadowing binding, the second reaches a `let` still in its TDZ and must throw.

function shouldThrowTDZ(f, what) {
    let threw = false;
    try {
        f();
    } catch (e) {
        threw = e instanceof ReferenceError;
    }
    if (!threw)
        throw new Error("expected a TDZ ReferenceError: " + what);
}

shouldThrowTDZ(function () {
    {
        var o = { x: 1 };
        with (o) { var a = x; delete o.x; var b = x; }
        let x = 2;
    }
    return [a, b];
}, "with object property deleted between reads");

shouldThrowTDZ(function () {
    {
        var r = (function () { eval("var x = 1"); var a = x; delete x; var b = x; return b; })();
        let x = 2;
    }
    return r;
}, "eval-declared var deleted between reads");

let answers = 0;
const proxy = new Proxy({}, { has(t, k) { return k === "x" && answers++ === 0; }, get() { return "from-proxy"; } });
shouldThrowTDZ(function () {
    {
        with (proxy) { var a = x; var b = x; }
        let x = "later";
    }
    return b;
}, "Proxy with-object answering has() once");

shouldThrowTDZ(function () {
    {
        var o = { x: "with" };
        with (o) {
            var r = (function inner() { var a = x; delete o.x; var b = x; return b; })();
        }
        let x = "later";
    }
    return r;
}, "with object in the enclosing function");

shouldThrowTDZ(function () {
    function mid() {
        eval("var X = 5");
        return (function inner() { let a = X; delete X; return [a, X]; })();
    }
    let r = mid();
    let X = 1;
    return r;
}, "eval var in the enclosing function deleted between reads");

// Storing a possibly-empty value (a derived constructor parking |this|) must not count as initialization.
class Base { constructor() { this.tag = "base"; } }
class Derived extends Base {
    constructor(touchFirst) {
        const peek = () => this.tag;
        if (touchFirst)
            shouldThrowTDZ(peek, "this before super()");
        super();
        if (peek() !== "base")
            throw new Error("bad this after super()");
    }
}
for (let i = 0; i < 100; ++i) {
    new Derived(false);
    new Derived(true);
}
