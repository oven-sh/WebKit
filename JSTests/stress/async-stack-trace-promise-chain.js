//@ requireOptions("--useAsyncStackTrace=1")

// The async frames of an Error are the async functions that wait for the job that made it. The walk goes from
// promise to promise: through then(), catch() and finally(), through a promise that another one was resolved with,
// through combinators inside each other, and it starts at the promise that a then() handler settles.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

// Throws late, so that every promise of a shape has its reaction by then.
async function failing() {
    for (let i = 0; i < 10; i++)
        await 0;
    throw new Error("failing");
}

function thrower() {
    throw new Error("thrower");
}

class SubPromise extends Promise { }

const shapes = {
    // The two shapes that had their callers before: an await, and an await of one combinator.
    async direct() { await failing(); },
    async all() { await Promise.all([failing()]); },
    async race() { await Promise.race([failing()]); },
    async promiseResolve() { await Promise.resolve(failing()); },

    // A promise that then(), catch() or finally() returned.
    async then() { await failing().then(v => v); },
    async thenBoth() { await failing().then(v => v, e => { throw e; }); },
    async catch_() { await failing().catch(e => { throw e; }); },
    async finally_() { await failing().finally(() => { }); },
    async thenWithoutHandlers() { await failing().then(); },
    async thenThen() { await failing().then(v => v).then(v => v); },
    async thenCatchFinally() { await failing().then(v => v).catch(e => { throw e; }).finally(() => { }); },

    // A promise that was resolved with the promise.
    async returnWithoutAwait() {
        async function inner() { return failing(); }
        await inner();
    },
    async returnThenWithoutAwait() {
        async function inner() { return failing().then(v => v); }
        await inner();
    },
    async resolveOfConstructor() { await new Promise(resolve => resolve(failing())); },
    async resolveOfWithResolvers() {
        const { promise, resolve } = Promise.withResolvers();
        resolve(failing());
        await promise;
    },
    async handlerReturnsPromise() { await Promise.resolve(1).then(() => failing()); },
    async finallyHandlerReturnsPromise() { await Promise.resolve(1).finally(() => failing()); },

    // A resolving function as the handler.
    async thenWithResolvingFunctions() {
        const { promise, resolve, reject } = Promise.withResolvers();
        failing().then(resolve, reject);
        await promise;
    },
    async thenable() { await { then(resolve, reject) { failing().then(resolve, reject); } }; },
    async thenableInThen() { await Promise.resolve(1).then(() => ({ then(resolve, reject) { failing().then(resolve, reject); } })); },

    // Combinators and then() inside each other.
    async allOfThen() { await Promise.all([failing().then(v => v)]); },
    async thenOfAll() { await Promise.all([failing()]).then(v => v); },
    async allSettledOfThen() { await Promise.allSettled([failing().then(v => v)]).then(([result]) => { throw result.reason; }); },
    async anyOfThen() { await Promise.any([failing().then(v => v)]).catch(e => { throw e.errors[0]; }); },
    async raceOfThen() { await Promise.race([failing().then(v => v)]); },
    async thenOfRace() { await Promise.race([failing()]).then(v => v); },
    async allOfAll() { await Promise.all([Promise.all([failing()])]); },
    async allOfRaceOfAll() { await Promise.all([Promise.race([Promise.all([failing()])])]); },

    // A subclass: then() settles a capability, and an await calls then().
    async subclassResolvedWithPromise() { await SubPromise.resolve(failing()); },
    async thenOnSubclass() { await SubPromise.resolve(failing()).then(v => v); },

    // An async generator.
    async asyncGeneratorAwaits() {
        async function* generator() { yield await failing(); }
        for await (const value of generator()) { }
    },
    async asyncGeneratorNext() {
        async function* generator() { yield await failing(); }
        await generator().next();
    },
    async asyncGeneratorYieldStar() {
        async function* inner() { yield await failing(); }
        async function* outer() { yield* inner(); }
        for await (const value of outer()) { }
    },

    // The Error is made in a handler: no async function is on the stack.
    async handlerThrows() { await Promise.resolve(1).then(thrower); },
    async catchHandlerThrows() { await Promise.reject(1).catch(thrower); },
    async bothHandlersGivenAndFulfillThrows() { await Promise.resolve(1).then(thrower, () => { }); },
    async finallyHandlerThrows() { await Promise.resolve(1).finally(thrower); },
    async handlerThrowsThenThen() { await Promise.resolve(1).then(thrower).then(v => v); },
    async handlerOfPendingPromiseThrows() {
        const { promise, resolve } = Promise.withResolvers();
        const result = promise.then(thrower);
        resolve(1);
        await result;
    },
    async handlerAfterCombinatorThrows() { await Promise.allSettled([Promise.resolve(1)]).then(thrower); },
    async boundHandlerThrows() { await Promise.resolve(1).then(thrower.bind(null)); },
    async handlerInForAwaitThrows() {
        async function* generator() { yield 1; }
        for await (const value of generator())
            await Promise.resolve(1).then(thrower);
    },
    async asyncHandlerThrows() {
        await Promise.resolve(1).then(async function thrower() {
            for (let i = 0; i < 10; i++)
                await 0;
            throw new Error("thrower");
        });
    },
    async thenOfThenableThrows() { await { then() { thrower(); } }; },
    async thenOfThenableInThenThrows() { await Promise.resolve(1).then(() => ({ then() { thrower(); } })); },
    async asyncGeneratorBodyThrows() {
        async function* generator() {
            await 0;
            thrower();
        }
        for await (const value of generator()) { }
    },
};

// Shapes whose callers are not in the trace. V8 has none for the first three either.
const withoutCallers = new Set([
    // The promise has two reactions.
    "twoReactions",
    "twoReactionsOlderIsAwaited",
    // The handler settles a capability, not a promise.
    "handlerOnSubclassThrows",
    // The resolve and reject functions of `new Promise` are closures here. V8 follows its own, which are not.
    "constructorFunctionsAsHandlers",
]);
Object.assign(shapes, {
    async twoReactions() {
        const promise = failing();
        promise.catch(() => { });
        await promise.then(v => v);
    },
    async twoReactionsOlderIsAwaited() {
        const promise = failing();
        const result = promise.then(v => v);
        promise.catch(() => { });
        await result;
    },
    async handlerOnSubclassThrows() { await SubPromise.resolve(1).then(thrower); },
    async constructorFunctionsAsHandlers() { await new Promise((resolve, reject) => failing().then(resolve, reject)); },
});

function framesOf(error) {
    const frames = [];
    for (const line of error.stack.split("\n")) {
        const name = line.split("@")[0];
        if (name === "drainMicrotasks")
            break;
        frames.push(name);
    }
    return frames.join(", ");
}

const loopCount = Math.min(testLoopCount, 20);
for (const [name, shape] of Object.entries(shapes)) {
    async function caller() { await shape(); }
    for (let i = 0; i < loopCount; i++) {
        let error;
        caller().then(() => { throw new Error(`${name} did not throw`); }, e => { error = e; });
        drainMicrotasks();
        if (!error)
            throw new Error(`${name} did not settle`);
        const frames = framesOf(error).split(", ");
        // The frames of the job itself: the function that throws, and the callback of `then` that calls it in two shapes.
        const firstAsync = frames.findIndex(frame => frame.startsWith("async "));
        const asyncFrames = firstAsync < 0 ? "" : frames.slice(firstAsync).join(", ");
        shouldBe(frames[0], error.message, `${name}: the function that throws`);
        const shapeName = shape.name;
        shouldBe(asyncFrames, withoutCallers.has(name) ? "" : `async ${shapeName}, async caller`, name);
    }
}

// A link from a promise to the promise that it settles adds no frame, so Error.stackTraceLimit does not bound the
// links. The walk follows a bounded number of them.
{
    function chained(count) {
        return async function chain() {
            let promise = failing();
            for (let i = 0; i < count; i++)
                promise = promise.then(v => v);
            await promise;
        };
    }
    function asyncFramesUnder(shape) {
        let error;
        shape().catch(e => { error = e; });
        drainMicrotasks();
        return error.stack.split("\n").filter(line => line.startsWith("async ")).map(line => line.split("@")[0]).join(", ");
    }
    shouldBe(asyncFramesUnder(chained(1000)), "async chain", "1000 links");
    shouldBe(asyncFramesUnder(chained(100000)), "", "100000 links");
}

// Promises that settle each other in a circle: the promise waits for its own then(). The walk ends.
{
    const { promise, resolve } = Promise.withResolvers();
    const derived = promise.then(v => v);
    derived.then(resolve);
    let stack;
    async function underCircle() {
        await 0;
        stack = new Error("circle").stack;
    }
    underCircle().then(resolve);
    drainMicrotasks();
    shouldBe(stack.split("\n")[0].split("@")[0], "underCircle", "circle of promises");
    shouldBe(stack.includes("async "), false, "circle of promises");
}

// The same through Promise.race, and with the circle closed by resolve(promise).
{
    let stack;
    async function underCircle() {
        for (let i = 0; i < 5; i++)
            await 0;
        stack = new Error("circle").stack;
    }
    let resolveOuter;
    const outer = new Promise(resolve => { resolveOuter = resolve; });
    const derived = outer.then(v => v);
    resolveOuter(Promise.race([derived, underCircle()]));
    drainMicrotasks();
    shouldBe(stack.split("\n")[0].split("@")[0], "underCircle", "circle through race");
    shouldBe(stack.includes("async "), false, "circle through race");
}

// An await cycle whose only async function adds no frame, through then(): Array.fromAsync awaits a race that holds
// a then() of its own promise.
{
    let stack;
    async function capture() {
        await 0;
        stack = new Error("cycle").stack;
    }
    const promise = Array.fromAsync([1, 2], (value, index) => index ? Promise.race([promise.then(v => v), capture()]) : value);
    drainMicrotasks();
    shouldBe(stack.split("\n")[0].split("@")[0], "capture", "await cycle through then()");
    Error.stackTraceLimit = Infinity;
    capture();
    drainMicrotasks();
    shouldBe(stack.split("\n")[0].split("@")[0], "capture", "await cycle through then(), no limit");
}
