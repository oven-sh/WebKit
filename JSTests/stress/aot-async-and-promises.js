//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
const direct = "direct-promise-operation";
function outcomeOf(promise) {
    let outcome = "pending";
    promise.then(value => { outcome = ["fulfilled", value]; }, reason => { outcome = ["rejected", reason]; });
    drainMicrotasks();
    return outcome;
}
function checkFulfilled(promise, expected, what) {
    check(promise instanceof Promise, true, what + " is a promise");
    check(Object.getPrototypeOf(promise), Promise.prototype, "the prototype of " + what);
    let outcome = outcomeOf(promise);
    check(outcome[0], "fulfilled", what);
    check(outcome[1], expected, what);
}
function checkRejected(promise, expected, what) {
    check(promise instanceof Promise, true, what + " is a promise");
    let outcome = outcomeOf(promise);
    check(outcome[0], "rejected", what);
    check(outcome[1], expected, what);
}

async function empty() { }
async function returns(value) { return value; }
async function throws(reason) { throw reason; }
async function returnsOrThrows(value, fails) { if (fails) throw value; return value; }
async function failsInParameters(value = (() => { throw "default"; })()) { return value; }
async function awaits(value) { let result = await value; return result; }
async function awaitsTwice(first, second) { return [await first, await second].join(); }
async function awaitsThenThrows(value) { await value; throw "after await"; }
async function awaitsAndCatches(value) { try { return await value; } catch (reason) { return "caught " + reason; } }
async function failsInParametersBeforeAwait(value = (() => { throw "default"; })()) { return await value; }
const arrow = async value => value;
const awaitingArrow = async value => await value;
const holder = { async method(value) { return [this === holder, await value].join(); } };
function notAsync(newResolvedPromise, value) { return newResolvedPromise(value); }
function makesPromise(value) { return new Promise(resolve => resolve(value)); }
for (let f of [empty, returns, throws, returnsOrThrows, failsInParameters, awaits, awaitsTwice, awaitsThenThrows, awaitsAndCatches, failsInParametersBeforeAwait, arrow, awaitingArrow, holder.method, notAsync, makesPromise])
    noInline(f);

const object = { plain: true };
const symbol = Symbol();
class Derived extends Promise { }
for (let round = 0; round < 4; ++round) {
    checkFulfilled(empty(), undefined, "the result of an empty async function");
    for (let value of [undefined, null, true, false, 0, -0, 1.5, NaN, "", "text", "rope" + round, 7n, 2n ** 70n, symbol, object, [], () => { }]) {
        checkFulfilled(returns(value), value, "the result of returning " + typeof value);
        checkFulfilled(arrow(value), value, "the result of an arrow returning " + typeof value);
        checkFulfilled(awaits(value), value, "the result of awaiting " + typeof value);
        checkFulfilled(awaitingArrow(value), value, "the result of an arrow awaiting " + typeof value);
        checkFulfilled(returnsOrThrows(value, false), value, "the result of returning " + typeof value);
        checkRejected(throws(value), value, "the result of throwing " + typeof value);
        checkRejected(returnsOrThrows(value, true), value, "the result of throwing " + typeof value);
        checkRejected(awaits(Promise.reject(value)), value, "the result of awaiting a rejected promise");
    }
    checkFulfilled(returns(Promise.resolve(5)), 5, "the result of returning a promise");
    checkFulfilled(returns(returns(returns(5))), 5, "the result of returning the result of an async function");
    checkRejected(returns(Promise.reject(5)), 5, "the result of returning a rejected promise");
    checkFulfilled(returns(Derived.resolve(5)), 5, "the result of returning an instance of a subclass of Promise");
    checkFulfilled(awaits(Derived.resolve(5)), 5, "the result of awaiting an instance of a subclass of Promise");
    checkFulfilled(returns({ then(resolve) { resolve(6); } }), 6, "the result of returning a thenable");
    checkFulfilled(awaits({ then(resolve) { resolve(6); } }), 6, "the result of awaiting a thenable");
    checkRejected(returns({ then(resolve, reject) { reject(6); } }), 6, "the result of returning a thenable that rejects");
    checkRejected(returns({ then() { throw 7; } }), 7, "the result of returning a thenable that throws");
    checkRejected(returns({ get then() { throw 8; } }), 8, "the result of returning an object whose then cannot be read");
    checkRejected(awaits({ get then() { throw 8; } }), 8, "the result of awaiting an object whose then cannot be read");
    checkFulfilled(returns({ then(resolve, reject) { resolve(9); reject(10); resolve(11); } }), 9, "the result of returning a thenable that settles three times");
    let withThen = { then: 1 };
    checkFulfilled(returns(withThen), withThen, "the result of returning an object whose then is not callable");
    checkRejected(failsInParameters(), "default", "the result of failing in the parameters");
    checkFulfilled(failsInParameters(1), 1, "the result of not failing in the parameters");
    checkRejected(failsInParametersBeforeAwait(), "default", "the result of failing in the parameters of a function that awaits");
    checkFulfilled(failsInParametersBeforeAwait(1), 1, "the result of not failing in the parameters of a function that awaits");
    checkFulfilled(awaitsTwice(1, returns(2)), "1,2", "the result of awaiting twice");
    checkRejected(awaitsTwice(1, throws(2)), 2, "the result of awaiting a rejection the second time");
    checkRejected(awaitsThenThrows(1), "after await", "the result of throwing after await");
    checkFulfilled(awaitsAndCatches(throws("reason")), "caught reason", "the result of catching a rejection");
    checkFulfilled(awaitsAndCatches(1), 1, "the result of catching nothing");
    checkFulfilled(holder.method(3), "true,3", "the result of an async method");
    check(notAsync(value => value + 1, 1), 2, "a parameter called newResolvedPromise");
    checkFulfilled(makesPromise(4), 4, "the result of new Promise");
    check(returns(1) === returns(1), false, "two results of an async function");
}
applies(empty, direct + ":operationNewResolvedPromise");
applies(returns, direct + ":operationNewResolvedPromise");
applies(returnsOrThrows, direct + ":operationNewResolvedPromise", direct + ":operationNewRejectedPromise");
applies(throws, direct + ":operationNewRejectedPromise");
applies(arrow, direct + ":operationNewResolvedPromise");
applies(failsInParameters, direct + ":operationNewRejectedPromise");
for (let f of [awaits, awaitsTwice, awaitingArrow, holder.method])
    applies(f, direct + ":operationResolvePromiseFirstResolving", direct + ":operationRejectPromiseFirstResolving", direct + ":operationAOTAsyncFunctionDrive");
applies(failsInParametersBeforeAwait, direct + ":operationRejectPromiseFirstResolving");
doesNotApply(notAsync, direct);
doesNotApply(makesPromise, direct);

let log = [];
async function logs(name, value) {
    log.push(name + " starts");
    let result = await value;
    log.push(name + " has " + result);
    result = await returns(result + 1);
    log.push(name + " has " + result);
    return result;
}
noInline(logs);
for (let round = 0; round < 3; ++round) {
    log = [];
    logs("a", 1).then(value => log.push("a is " + value));
    Promise.resolve().then(() => log.push("job"));
    logs("b", Promise.resolve(10)).then(value => log.push("b is " + value));
    logs("c", { then(resolve) { log.push("then of c"); resolve(20); } }).then(value => log.push("c is " + value));
    returns(0).then(() => log.push("returned"));
    returns(Promise.resolve(0)).then(() => log.push("returned a promise"));
    log.push("end of script");
    drainMicrotasks();
    check(log.join("; "), "a starts; b starts; c starts; end of script; a has 1; job; b has 10; then of c; returned; a has 2; b has 11; c has 20; a is 2; b is 11; c has 21; returned a promise; c is 21", "the order of jobs");
}

function* counts(limit) { for (let i = 0; i < limit; ++i) yield i; }
async function* countsSlowly(limit) { for (let i = 0; i < limit; ++i) yield await i; }
async function sums(limit) { let sum = 0; for await (let i of countsSlowly(limit)) sum += i; return sum; }
async function manyAwaits(count) { let sum = 0; for (let i = 0; i < count; ++i) sum += await returns(i); return sum; }
function manyPromises(count) { let all = []; for (let i = 0; i < count; ++i) all.push(returns(i), awaits("s" + i), throws(i)); return all; }
for (let f of [counts, countsSlowly, sums, manyAwaits, manyPromises])
    noInline(f);
for (let round = 0; round < 3; ++round) {
    check([...counts(5)].join(), "0,1,2,3,4", "a generator");
    checkFulfilled(sums(5), 10, "the sum of an async generator");
    checkFulfilled(manyAwaits(300), 44850, "the sum of many awaited values");
    let all = manyPromises(300);
    gc();
    for (let i = 0; i < 300; ++i) {
        if (i % 50 == 25)
            edenGC();
        checkFulfilled(all[3 * i], i, "one of many promises");
        checkFulfilled(all[3 * i + 1], "s" + i, "one of many promises");
        checkRejected(all[3 * i + 2], i, "one of many promises");
    }
}

let stack = "";
async function fails() { await 1; throw new Error("in an async function"); }
async function callsFails() { await fails(); }
noInline(fails);
noInline(callsFails);
callsFails().catch(error => { stack = error.stack; });
drainMicrotasks();
check(stack.split("\n")[0].split("@")[0], "fails", "the stack of an error thrown after await");
function failsAtOnce() { throw new Error("at once"); }
async function callsFailsAtOnce() { failsAtOnce(); }
noInline(failsAtOnce);
noInline(callsFailsAtOnce);
callsFailsAtOnce().catch(error => { stack = error.stack; });
drainMicrotasks();
check(stack.split("\n").map(line => line.split("@")[0]).join(), "failsAtOnce,callsFailsAtOnce,global code", "the stack of an error thrown below an async function");
