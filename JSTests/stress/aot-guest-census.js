//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1")
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
const counts = typeof aotOperationCount === "function" && aotOperationCount("guest:Function") !== null;
const kinds = ["Function", "eval", "direct-eval", "import", "ran-global", "ran-eval", "ran-function", "ran-module"];
function expect(what, run, expected) {
    let before = kinds.map(kind => counts ? aotOperationCount("guest:" + kind) : 0);
    run();
    if (!counts)
        return;
    let changed = [];
    kinds.forEach((kind, i) => {
        let difference = aotOperationCount("guest:" + kind) - before[i];
        if (difference)
            changed.push(kind + " " + difference);
    });
    check(changed.join(", "), expected, what);
}

function ofTheProgram(x) { return [x].map(y => y + 1)[0]; }
expect("functions of the program and built-ins", () => { ofTheProgram(1); ofTheProgram(2); }, "");

let made;
expect("a function is made and not called", () => { made = new Function("x", "return x + 1;"); }, "Function 1");
expect("its first call", () => { made(1); }, "ran-function 1");
expect("its second call", () => { made(2); }, "");
expect("made without new, called at once", () => { Function("return 1;")(); }, "Function 1, ran-function 1");
expect("a function inside a function that was made", () => { new Function("return function () { return 1; };")()(); }, "Function 1, ran-function 2");
expect("a function that was made calls one that has not run yet", () => { new Function("function inner() { return 1; } return inner() + inner();")(); }, "Function 1, ran-function 2");
expect("the other constructors", () => {
    new (Object.getPrototypeOf(function* () { }).constructor)("yield 1;");
    new (Object.getPrototypeOf(async function () { }).constructor)("await 1;");
}, "Function 2");

expect("indirect eval", () => { (0, eval)("1 + 1"); }, "eval 1, ran-eval 1");
expect("evaluated code calls a function that has not run yet", () => { (0, eval)("function evaluatedInner() { return 2; } evaluatedInner() + evaluatedInner()"); }, "eval 1, ran-eval 1, ran-function 1");
expect("indirect eval of a literal runs no code", () => { (0, eval)("[1, 2]"); }, "eval 1");
expect("eval of something that is not a string", () => { (0, eval)(1); }, "");
function evaluates(x) { return eval("x + 1"); }
expect("direct eval", () => { evaluates(1); }, "direct-eval 1, ran-eval 1");

expect("an import by name", () => { import("./this-file-does-not-exist.js").catch(() => { }); drainMicrotasks(); }, "import 1");
