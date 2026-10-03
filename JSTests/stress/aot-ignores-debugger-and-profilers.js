//@ runDefault("--compileMainScriptAheadOfTime=1", "--forceDebuggerBytecodeGeneration=1", "--alwaysUseShadowChicken=1")
//@ run("type-profiler", "--useTypeProfiler=1")
//@ run("control-flow-profiler", "--useControlFlowProfiler=1")
//@ run("fuzzing-and-validation", "--returnEarlyFromInfiniteLoopsForFuzzing=1", "--validateBytecode=1", "--collectContinuously=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function repeat(f) {
    for (let i = 0; i < 100; i++)
        f(i);
}

function sumBelow(n) { return n ? n * (n - 1) / 2 : 0; }
function adds(a, b) { return a + b; }
function stops(a) { debugger; return a * 2; }
function loops(n) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += i;
    return sum;
}
function callsInTailPosition(n) {
    "use strict";
    return n ? callsInTailPosition(n - 1) : "done";
}
function collects(depth) {
    if (!depth) {
        fullGC();
        return 0;
    }
    return collects(depth - 1) + 1;
}
function throwsAndCatches(value) {
    try {
        throw new Error(value);
    } catch (error) {
        return error.message;
    }
}
async function awaits(value) {
    for (let i = 0; i < 10; i++)
        value += await i;
    return value;
}
function* yields(n) {
    for (let i = 0; i < n; i++)
        yield i;
}
class Point {
    constructor(x, y) { this.x = x; this.y = y; }
    get sum() { return this.x + this.y; }
}
const functions = [adds, stops, loops, callsInTailPosition, collects, throwsAndCatches, awaits, yields, Point];
for (const f of functions)
    noInline(f);
for (const f of functions)
    check(isAOTCompiled(f), true, f.name + " is compiled");

repeat(i => {
    check(adds(i, 1), i + 1, "a sum");
    check(adds("a", i), "a" + i, "a sum of a string and a number");
    check(stops(i), i * 2, "a debugger statement");
    check(loops(i), sumBelow(i), "a loop");
    check(callsInTailPosition(i), "done", "a call in tail position");
    check(throwsAndCatches("e" + i), "e" + i, "an exception");
    check([...yields(4)].join(), "0,1,2,3", "a generator");
    check(new Point(i, 2).sum, i + 2, "a class");
});
check(collects(50), 50, "a collection below many frames");
check(callsInTailPosition(100000), "done", "many calls in tail position");

repeat(i => {
    check(eval("debugger; adds(i, 2)"), i + 2, "code that is evaluated");
    check(new Function("a", "debugger; let sum = 0; for (let i = 0; i < a; i++) sum += i; return sum;")(i), sumBelow(i), "a function that is made from text");
    check((0, eval)("(function tail(n) { 'use strict'; return n ? tail(n - 1) : collects(3); })")(20), 3, "a collection below code that is evaluated");
});

let awaited;
awaits(5).then(value => { awaited = value; });
drainMicrotasks();
check(awaited, 50, "an async function");
