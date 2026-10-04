//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "-m")
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
let log = [];
function makeAsyncFunctions(count) {
    let all = [];
    for (let i = 0; i < count; ++i)
        all.push(new Function("log", "return async function f" + i + "(x) { log.push('a" + i + "'); let y = await x; log.push('b" + i + "'); return y + " + i + "; }")(log));
    return all;
}
async function a0(x) { return (await x) + 0; } async function a1(x) { return (await x) + 1; } async function a2(x) { return (await x) + 2; } async function a3(x) { return (await x) + 3; }
async function a4(x) { return (await x) + 4; } async function a5(x) { return (await x) + 5; } async function a6(x) { return (await x) + 6; } async function a7(x) { return (await x) + 7; }
async function a8(x) { return (await x) + 8; } async function a9(x) { return (await x) + 9; } async function a10(x) { return (await x) + 10; } async function a11(x) { return (await x) + 11; }
const compiled = [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11];

let total = 0;
for (let round = 0; round < 50; ++round) {
    for (let f of compiled)
        f(round).then(value => { total += value; });
}
drainMicrotasks();
check(total, 50 * 66 + 12 * (49 * 50 / 2), "twelve async functions in flight");

const interpreted = makeAsyncFunctions(3);
let mixed = [];
for (let i = 0; i < 3; ++i) {
    interpreted[i](10).then(value => mixed.push(value));
    compiled[i](20).then(value => mixed.push(value));
}
drainMicrotasks();
check(mixed.join(), "10,20,11,21,12,22", "interpreted and compiled functions in turn");
check(log.join(), "a0,a1,a2,b0,b1,b2", "the interpreted ones ran in order");

let seen = [];
const settled = Promise.resolve("v");
settled.then(() => seen.push("none"));
settled.then(value => seen.push(value));
settled.then((value, extra) => seen.push(value + extra));
settled.then((value, extra, more) => seen.push(value + extra + more));
settled.then((...all) => seen.push(all.length + all[0]));
settled.then(function () { seen.push(arguments.length + arguments[0]); });
settled.then(function (value = "d", other = "e") { seen.push(value + other); });
drainMicrotasks();
check(seen.join(), "none,v,vundefined,vundefinedundefined,1v,1v,ve", "handlers with fewer and more parameters than arguments");

async function throwsAfterAwait(x) { await x; throw new RangeError("after"); }
async function catchesAfterAwait(x) { try { await throwsAfterAwait(x); } catch (error) { return error.message; } finally { seen.push("finally"); } }
seen = [];
catchesAfterAwait(1).then(value => seen.push(value));
throwsAfterAwait(1).catch(error => seen.push(error.constructor.name));
drainMicrotasks();
check(seen.join(), "finally,RangeError,after", "exceptions after a resumption");

async function namesAfterAwait() { await 0; return new Error("x").stack.split("\n").map(line => line.split("@")[0]).slice(0, 2).join(); }
async function awaitsNames() { return await namesAfterAwait(); }
let names;
awaitsNames().then(value => { names = value; });
drainMicrotasks();
check(names, "namesAfterAwait,async awaitsNames", "the stack of a resumed function");

async function recursesAfterAwait() { await 0; function deep(n) { return deep(n + 1) + 1; } try { deep(0); } catch (error) { return error instanceof RangeError; } }
let overflowed;
recursesAfterAwait().then(value => { overflowed = value; });
drainMicrotasks();
check(overflowed, true, "a stack overflow below a resumed function");

async function* generates() { yield 1; yield await 2; yield* [3, 4]; }
let collected = [];
(async () => { for await (let value of generates()) collected.push(value); })();
drainMicrotasks();
check(collected.join(), "1,2,3,4", "an async generator");

let kept = compiled.map(f => f(1));
gc();
let afterCollection = 0;
for (let promise of kept)
    promise.then(value => { afterCollection += value; });
drainMicrotasks();
check(afterCollection, 12 + 66, "a collection between the await and the resumption");
