//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1")
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
const counts = typeof aotOperationCount === "function" && aotOperationCount("await") !== null;
const names = ["job-call:static-code", "job-call:other"];
for (let where of ["await:first-segment", "await:resumed", "async-generator-await:synchronous", "async-generator-await:job"]) {
    for (let queue of ["queue-empty", "queue-not-empty"]) {
        for (let operand of ["not-an-object", "pending", "fulfilled", "rejected", "other-object"])
            names.push(where + ":" + queue + ":" + operand);
    }
}
function count(prefix) {
    let total = 0;
    for (let name of names) {
        if (name === prefix || name.startsWith(prefix + ":"))
            total += aotOperationCount(name);
    }
    return total;
}
function expect(what, run, expected) {
    drainMicrotasks();
    let before = { };
    for (let name in expected)
        before[name] = counts ? count(name) : 0;
    run();
    drainMicrotasks();
    if (!counts)
        return;
    for (let name in expected)
        check(count(name) - before[name], expected[name], what + ", " + name);
}
function later() { Promise.resolve().then(() => { }); }
async function steps(count, operand) { for (let i = 0; i < count; ++i) await operand; }
const fulfilled = Promise.resolve(1);
const rejected = Promise.reject(1);
rejected.catch(() => { });

expect("alone", () => { steps(4, 0); }, {
    "await": 4, "await:first-segment": 1, "await:first-segment:queue-empty:not-an-object": 1,
    "await:resumed": 3, "await:resumed:queue-empty": 3, "await:resumed:queue-empty:not-an-object": 3, "await:resumed:queue-not-empty": 0,
    "job-call:static-code": 4, "job-call:other": 0 });
expect("alone, a fulfilled promise", () => { steps(3, fulfilled); }, {
    "await": 3, "await:first-segment:queue-empty:fulfilled": 1, "await:resumed:queue-empty:fulfilled": 2, "await:resumed:queue-empty:not-an-object": 0 });
expect("two in turn", () => { steps(3, 0); steps(3, 0); }, {
    "await": 6, "await:first-segment:queue-empty": 1, "await:first-segment:queue-not-empty": 1, "await:resumed:queue-not-empty": 4, "await:resumed:queue-empty": 0,
    "job-call:static-code": 6 });
expect("a job of its own first", () => { (async () => { await 0; later(); await 0; later(); await fulfilled; })(); }, {
    "await": 3, "await:first-segment": 1, "await:resumed:queue-not-empty:not-an-object": 1, "await:resumed:queue-not-empty:fulfilled": 1, "await:resumed:queue-empty": 0 });
expect("by turns", () => { (async () => { for (let i = 0; i < 6; ++i) { if (i & 1) later(); await i; } })(); }, {
    "await": 6, "await:first-segment": 1, "await:resumed:queue-empty": 2, "await:resumed:queue-not-empty": 3 });
let settle;
expect("a pending promise", () => { (async () => { await 0; await new Promise(resolve => { settle = resolve; }); })(); drainMicrotasks(); settle(1); }, {
    "await": 2, "await:resumed:queue-empty:pending": 1 });
expect("a rejected promise", () => { (async () => { await 0; try { await rejected; } catch { } })(); }, {
    "await": 2, "await:resumed:queue-empty:rejected": 1 });
expect("an object with a then method", () => { (async () => { await 0; await { then(resolve) { resolve(1); } }; })(); }, {
    "await": 2, "await:resumed:queue-empty:other-object": 1 });
expect("no await at all", () => { (async () => 1)(); }, { "await": 0, "job-call": 0 });

expect("a compiled handler", () => { fulfilled.then(value => value + 1); }, { "job-call:static-code": 1, "job-call:other": 0 });
expect("a native handler", () => { fulfilled.then(Math.abs); }, { "job-call:static-code": 0, "job-call:other": 1 });
const interpreted = new Function("return async function (x) { await x; await x; }")();
expect("an interpreted async function", () => { interpreted(0); }, {
    "await": 2, "await:first-segment": 1, "await:resumed": 1, "job-call:static-code": 0, "job-call:other": 2 });

async function* generates() { yield 1; await 0; await fulfilled; yield 2; }
expect("an async generator", () => { (async () => { for await (let value of generates()); })(); }, { "async-generator-await": 2 });
