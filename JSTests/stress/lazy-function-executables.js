// Options::useLazyFunctionExecutables() / Options::useThinChildExecutables() (both default on): a CodeBlock creates a
// function's FunctionExecutable at its first new_func*, and a function decoded from the bytecode cache keeps its name and
// end positions in the cache until introspection. The default modes run this from source and (bytecode-cache) from a cache.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

// new_func* for the same index before and after the enclosing function tiers up.
function makeClosures(i) {
    function decl(a, b) { return a + b + i; }
    let expr = function namedExpr(a) { return a * i; };
    let arrow = (a, b, c) => a - i;
    async function asyncDecl(x) { return x + i; }
    function* gen(x, y) { yield x + i; }
    let method = { m(p, q) { return p + q + i; } }.m;
    class K { constructor(z) { this.z = z + i; } static s() { return i; } }
    if (i % 100 == 99)
        return [decl, expr, arrow, asyncDecl, gen, method, K];
    return decl(1, 2) + expr(2) + arrow(3) + method(1, 1);
}
noInline(makeClosures);
let kept;
for (let i = 0; i < testLoopCount; ++i) {
    let r = makeClosures(i);
    if (typeof r == "number")
        shouldBe(r, 3 + i + 2 * i + 3 - i + 2 + i, "sum at " + i);
    else
        kept = r;
}

// Introspection of functions that were never called.
{
    let [decl, expr, arrow, asyncDecl, gen, method, K] = kept;
    shouldBe(decl.name, "decl", "decl.name");
    shouldBe(expr.name, "namedExpr", "expr.name");
    shouldBe(arrow.name, "arrow", "arrow.name");
    shouldBe(asyncDecl.name, "asyncDecl", "asyncDecl.name");
    shouldBe(gen.name, "gen", "gen.name");
    shouldBe(method.name, "m", "method.name");
    shouldBe(K.name, "K", "K.name");
    shouldBe(decl.length, 2, "decl.length");
    shouldBe(arrow.length, 3, "arrow.length");
    shouldBe(asyncDecl.length, 1, "asyncDecl.length");
    shouldBe(gen.length, 2, "gen.length");
    shouldBe(K.length, 1, "K.length");
    shouldBe(asyncDecl.toString(), "async function asyncDecl(x) { return x + i; }", "asyncDecl.toString()");
    shouldBe(gen.toString(), "function* gen(x, y) { yield x + i; }", "gen.toString()");
    shouldBe(K.toString(), "class K { constructor(z) { this.z = z + i; } static s() { return i; } }", "K.toString()");
    shouldBe(String(Object.getOwnPropertyDescriptor(K, "s").value), "s() { return i; }", "K.s source");
    // Called first, introspected after.
    shouldBe(typeof new K(1).z, "number", "K constructed");
    shouldBe(K.s.name, "s", "K.s.name");
}

// A function that is called but never introspected, then its source is asked for.
function outer() {
    function calledOnly(a, b, c, d) {
        return a + b + c + d;
    }
    return calledOnly;
}
let calledOnly = outer();
shouldBe(calledOnly(1, 2, 3, 4), 10, "calledOnly()");
shouldBe(calledOnly.toString(), "function calledOnly(a, b, c, d) {\n        return a + b + c + d;\n    }", "calledOnly.toString()");
shouldBe(calledOnly.length, 4, "calledOnly.length");

// Eval declares its functions right after linking.
function evalDeclares(n) {
    eval("function fromEval() { return " + n + "; } var alsoFromEval = function () { return fromEval() + 1; };");
    return fromEval() + alsoFromEval();
}
noInline(evalDeclares);
for (let i = 0; i < 200; ++i)
    shouldBe(evalDeclares(i), 2 * i + 1, "evalDeclares");

// Function declarations in a global-code loop body (the program CodeBlock itself tiers up while new_func runs).
var total = 0;
for (var i = 0; i < testLoopCount; ++i) {
    function inLoop(x) { return x + 1; }
    total += (function (y) { return inLoop(y); })(i);
}
shouldBe(total, testLoopCount * (testLoopCount + 1) / 2, "global loop total");

// Bound functions and hasInstance read .name / .length / prototype of a never-called target.
function neverCalledTarget(a, b, c) { return this; }
let bound = neverCalledTarget.bind(null, 1);
shouldBe(bound.name, "bound neverCalledTarget", "bound.name");
shouldBe(bound.length, 2, "bound.length");
shouldBe({} instanceof neverCalledTarget, false, "instanceof never-called");
