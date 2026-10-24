//@ requireOptions("--useAsyncStackTrace=1")
// Not the eager configurations: they compile each function of the table on the main thread, and the walk is not JIT code.
//@ defaultNoEagerRun

// The walk decodes one reaction at a time, so what it does for a wrapper must not depend on what the promise was
// wrapped in before, or on where the Error was made. This test has each wrapper alone, each wrapper around each
// kind of promise that another wrapper returns, and wrappers around each place where a job can make an Error.
// V8 (Node v26.3) has the same frames in every cell, except where a comment says that it has fewer.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

class SubPromise extends Promise { }

async function tick(count) {
    for (let i = 0; i < count; i++)
        await 0;
}

// Throws late, so that every promise of a cell has its reaction by then.
async function failing() {
    await tick(12);
    throw new Error("failing");
}

function thrower() {
    throw new Error("thrower");
}

// The names of the async functions that the cells are made of. Other frames do not count: a frame of JSC's own
// code, and the frame of a function with no name.
const namedFrames = new Set(["shape", "caller", "mid", "loop", "loop2"]);

function asyncFramesOf(error) {
    const frames = [];
    for (const line of String(error.stack).split("\n")) {
        if (!line.startsWith("async "))
            continue;
        const name = line.slice("async ".length).split("@")[0];
        if (namedFrames.has(name))
            frames.push(name);
    }
    return frames.join(", ");
}

function asyncFramesUnder(makePromise, message) {
    async function shape() { await makePromise(); }
    async function caller() { await shape(); }
    let error;
    caller().then(() => { throw new Error(`${message} did not throw`); }, e => { error = e; });
    drainMicrotasks();
    if (!error)
        throw new Error(`${message} did not settle`);
    return asyncFramesOf(error);
}

const wrappers = {
    then: promise => promise.then(v => v),
    catchRethrow: promise => promise.catch(e => { throw e; }),
    finally: promise => promise.finally(() => { }),
    thenEmpty: promise => promise.then(),
    all: promise => Promise.all([promise]),
    allSecond: promise => Promise.all([Promise.resolve(0), promise]),
    race: promise => Promise.race([promise]),
    any: promise => Promise.any([promise]).catch(e => { throw e.errors[0]; }),
    allSettled: promise => Promise.allSettled([promise]).then(([result]) => { throw result.reason; }),
    resolve: promise => Promise.resolve(promise),
    newResolve: promise => new Promise(resolve => resolve(promise)),
    newThen: promise => new Promise((resolve, reject) => { promise.then(resolve, reject); }),
    withResolvers: promise => {
        const { promise: other, resolve } = Promise.withResolvers();
        resolve(promise);
        return other;
    },
    thenResolvers: promise => {
        const { promise: other, resolve, reject } = Promise.withResolvers();
        promise.then(resolve, reject);
        return other;
    },
    asyncReturn: promise => (async () => promise)(),
    asyncAwait: promise => (async function mid() { return await promise; })(),
    handlerReturns: promise => Promise.resolve(1).then(() => promise),
    finallyReturns: promise => Promise.resolve(1).finally(() => promise),
    thenable: promise => ({ then(resolve, reject) { promise.then(resolve, reject); } }),
    subResolve: promise => SubPromise.resolve(promise),
    subThen: promise => SubPromise.resolve(promise).then(v => v),
    subAll: promise => SubPromise.all([promise]),
    subAllSettled: promise => SubPromise.allSettled([promise]).then(([result]) => { throw result.reason; }),
    subAny: promise => SubPromise.any([promise]).catch(e => { throw e.errors[0]; }),
    subRace: promise => SubPromise.race([promise]),
    forAwaitSync: promise => (async function loop() { for await (const value of [promise]) { } })(),
    asyncGenerator: promise => (async function loop2() {
        async function* generator() { yield await promise; }
        for await (const value of generator()) { }
    })(),
    asyncGeneratorNext: promise => (async function* generator() { yield await promise; })().next(),
    asyncGeneratorYield: promise => (async function* generator() { yield promise; })().next(),
    yieldStarSync: promise => (async function* generator() { yield* [promise]; })().next(),
    // V8 has no callers through these two. A promise with two reactions: the await is the newest one.
    twoReactionsNewestAwait: promise => {
        promise.catch(() => { });
        return promise;
    },
    fromAsync: promise => Array.fromAsync([promise]),
    tryWrap: promise => Promise.try(() => promise),
};

// The async functions with a name that a wrapper adds to the chain.
const frameOfWrapper = { asyncAwait: "mid", forAwaitSync: "loop", asyncGenerator: "loop2" };

// These wrappers make a reaction that is a link to another promise. Such a link counts only as the one reaction
// of its promise, so it does not count after twoReactionsNewestAwait. The reaction of an await (in an async function
// or in an async generator), of a `for await` and of a combinator counts as the newest one. A combinator of a
// subclass is in the list: it takes SubPromise.resolve(promise) as its element.
const linksToPromise = new Set(["then", "catchRethrow", "finally", "thenEmpty", "newResolve", "newThen", "withResolvers", "thenResolvers",
    "asyncReturn", "handlerReturns", "finallyReturns", "subResolve", "subThen", "subAll", "subAllSettled", "subAny", "subRace", "fromAsync"]);

// These call a method of the promise, which the object that `thenable` returns does not have.
const callsMethodOfPromise = new Set(["then", "catchRethrow", "finally", "thenEmpty", "twoReactionsNewestAwait"]);

function expectedFrames(chain) {
    if (chain.length === 2 && chain[0] === "twoReactionsNewestAwait" && linksToPromise.has(chain[1]))
        return "";
    return [...chain.map(name => frameOfWrapper[name]).filter(Boolean), "shape", "caller"].join(", ");
}

// What the second wrapper of a pair gets from the first: a promise, the promise of a subclass, an object with a
// then() that is no promise, the promise of an async function, of a combinator, of the next() of an async generator
// and of `new Promise`, and a promise that has two reactions.
const firstWrappers = ["then", "subThen", "thenable", "asyncAwait", "all", "asyncGeneratorNext", "newThen", "twoReactionsNewestAwait"];

const wrapperNames = Object.keys(wrappers);
const chains = wrapperNames.map(name => [name]);
for (const first of firstWrappers) {
    for (const second of wrapperNames) {
        if (first !== "thenable" || !callsMethodOfPromise.has(second))
            chains.push([first, second]);
    }
}
shouldBe(chains.length, wrapperNames.length * (firstWrappers.length + 1) - callsMethodOfPromise.size, "cells of wrappers");
for (const chain of chains) {
    const name = chain.join(" > ");
    const frames = asyncFramesUnder(() => chain.reduce((promise, wrapper) => wrappers[wrapper](promise), failing()), name);
    shouldBe(frames, expectedFrames(chain), name);
}

// Where a job makes the Error: a handler, and what a handler calls through code of JSC that calls back.
const origins = {
    thenHandler: () => tick(6).then(thrower),
    catchHandler: () => tick(6).then(() => Promise.reject(1)).catch(thrower),
    finallyHandler: () => tick(6).finally(thrower),
    finallyHandlerAfterRejection: () => tick(6).then(() => { throw 1; }).finally(thrower),
    boundHandler: () => tick(6).then(thrower.bind(null)),
    proxyHandler: () => tick(6).then(new Proxy(thrower, { })),
    handlerCallsReviver: () => tick(6).then(() => JSON.parse("1", thrower)),
    handlerCallsSort: () => tick(6).then(() => [2, 1].sort(thrower)),
    handlerCallsForEach: () => tick(6).then(() => [1].forEach(thrower)),
    handlerCallsReflectApply: () => tick(6).then(() => Reflect.apply(thrower, null, [])),
    handlerCallsReplace: () => tick(6).then(() => "a".replace(/a/, thrower)),
    handlerCallsGetter: () => tick(6).then(() => ({ get x() { return thrower(); } }).x),
    handlerCallsToString: () => tick(6).then(() => `${{ toString: thrower }}`),
    handlerCallsConstructor: () => tick(6).then(() => new (class { constructor() { thrower(); } })()),
    handlerCallsIterator: () => tick(6).then(() => new Map({ [Symbol.iterator]: thrower })),
    handlerCallsEval: () => tick(6).then(() => (0, eval)("(function viaEval(f) { f(); })")(thrower)),
    thenOfThenable: () => tick(6).then(() => ({ then() { thrower(); } })),
    asyncHandler: () => Promise.resolve(1).then(async () => { await tick(6); thrower(); }),
    executorAfterAwait: () => (async () => { await tick(6); return new Promise(thrower); })(),
    asyncGeneratorBody: () => (async function* generator() { await tick(6); thrower(); })().next(),
    asyncGeneratorBodyAfterYield: () => (async () => {
        async function* generator() { yield 1; await tick(6); thrower(); }
        for await (const value of generator()) { }
    })(),
    handlerAfterAll: () => Promise.all([tick(6)]).then(thrower),
    handlerAfterRace: () => Promise.race([tick(6)]).then(thrower),
};

// No callers, and V8 has them for the first one.
const originsWithoutCallers = {
    // The getter runs when the job resolves the promise with what the handler returned. That call is a VM entry
    // of its own, and it has no context.
    getterOfThen: () => tick(6).then(() => ({ get then() { return thrower(); } })),
    // The handler settles a capability, not a promise.
    handlerOnSubclass: () => SubPromise.resolve(tick(6)).then(thrower),
};

const wrappersOfOrigins = ["then", "all", "asyncAwait", "newThen"];
for (const [originName, origin] of Object.entries(origins)) {
    shouldBe(asyncFramesUnder(origin, originName), "shape, caller", originName);
    for (const wrapper of wrappersOfOrigins) {
        const name = `${originName} > ${wrapper}`;
        shouldBe(asyncFramesUnder(() => wrappers[wrapper](origin()), name), expectedFrames([wrapper]), name);
    }
}
for (const [originName, origin] of Object.entries(originsWithoutCallers)) {
    shouldBe(asyncFramesUnder(origin, originName), "", originName);
    for (const wrapper of wrappersOfOrigins) {
        const name = `${originName} > ${wrapper}`;
        shouldBe(asyncFramesUnder(() => wrappers[wrapper](origin()), name), "", name);
    }
}
