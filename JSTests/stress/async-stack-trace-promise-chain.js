//@ requireOptions("--useAsyncStackTrace=1")
// Not the eager configurations: they compile each function of the table on the main thread, and the walk is not JIT code.
//@ defaultNoEagerRun

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

    // A resolve function as the handler.
    async thenWithResolvingFunctions() {
        const { promise, resolve, reject } = Promise.withResolvers();
        failing().then(resolve, reject);
        await promise;
    },
    async constructorFunctionsAsHandlers() { await new Promise((resolve, reject) => failing().then(resolve, reject)); },
    async constructorFunctionsInThenAndCatch() { await new Promise((resolve, reject) => { failing().then(resolve).catch(reject); }); },
    async constructorFunctionsOfSubclass() { await new SubPromise((resolve, reject) => failing().then(resolve, reject)); },
    async constructorFunctionsKept() {
        let resolve, reject;
        const promise = new Promise((resolveFunction, rejectFunction) => {
            resolve = resolveFunction;
            reject = rejectFunction;
        });
        failing().then(resolve, reject);
        await promise;
    },
    async thenable() { await { then(resolve, reject) { failing().then(resolve, reject); } }; },
    async thenableInResolve() { await Promise.resolve({ then(resolve, reject) { failing().then(resolve, reject); } }); },
    async thenableInThen() { await Promise.resolve(1).then(() => ({ then(resolve, reject) { failing().then(resolve, reject); } })); },

    // A function of JSC that is no resolve function, as the handler: what then() returned counts.
    async thenWithHostFunction() { await failing().then(Math.abs); },
    async thenWithBuiltinFunction() { await failing().then(Array.from); },

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

// Shapes whose callers are not in the trace. V8 has none for them either.
const withoutCallers = new Set([
    // The promise has two reactions.
    "twoReactions",
    "twoReactionsOlderIsAwaited",
    // The handler is a resolve function of a promise that nothing waits for. What then() returned does not count.
    "resultOfThenWithResolvingFunctions",
    // A reject function is a handler like any other: what then() returned counts, and nothing waits for it.
    "rejectFunctionAsFulfillHandler",
    "constructorRejectFunctionAsFulfillHandler",
    "rejectFunctionAsOnlyHandler",
    // The handler settles a capability, not a promise.
    "handlerOnSubclassThrows",
    // return(promise) of an async generator waits for the promise.
    "returnOfAsyncGenerator",
    "returnOfAsyncGeneratorAtYield",
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
    async resultOfThenWithResolvingFunctions() {
        const { promise, resolve, reject } = Promise.withResolvers();
        await failing().then(resolve, reject);
        await promise;
    },
    async rejectFunctionAsFulfillHandler() {
        const { promise, reject } = Promise.withResolvers();
        failing().then(reject, reject);
        await promise;
    },
    async constructorRejectFunctionAsFulfillHandler() { await new Promise((resolve, reject) => failing().then(reject, reject)); },
    async rejectFunctionAsOnlyHandler() { await new Promise((resolve, reject) => failing().catch(reject)); },
    async handlerOnSubclassThrows() { await SubPromise.resolve(1).then(thrower); },
    async returnOfAsyncGenerator() {
        async function* generator() { }
        await generator().return(failing());
    },
    async returnOfAsyncGeneratorAtYield() {
        async function* generator() { yield 1; }
        const iterator = generator();
        await iterator.next();
        await iterator.return(failing());
    },
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

function checkShapes(loopCount, state) {
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
            shouldBe(frames[0], error.message, `${name}, ${state}: the function that throws`);
            const shapeName = shape.name;
            shouldBe(asyncFrames, withoutCallers.has(name) ? "" : `async ${shapeName}, async caller`, `${name}, ${state}`);
        }
    }
}

// Bun calls the walk for a promise that native code rejects, and $vm.asyncStackTraceForNativeRejection(promise) is
// that call. It follows the links above, and it keeps what the walk that Bun had followed
// (AsyncStackTraceRule::NativeRejection).
let checkNativeShapes;
{
    const nativeShapes = {
        async direct(promise) { await promise; },
        async then(promise) { await promise.then(v => v); },
        async thenCatchFinally(promise) { await promise.then(v => v).catch(e => { throw e; }).finally(() => { }); },
        async returnWithoutAwait(promise) {
            async function inner() { return promise; }
            await inner();
        },
        async all(promise) { await Promise.all([promise]); },
        async allOfThen(promise) { await Promise.all([promise.then(v => v)]); },
        async thenWithResolvingFunctions(promise) {
            const { promise: other, resolve, reject } = Promise.withResolvers();
            promise.then(resolve, reject);
            await other;
        },
        // The newest reaction counts, however many the promise has.
        async newestOfTwoReactions(promise) {
            promise.catch(() => { });
            await promise.then(v => v);
        },
        // The promise that then() returned counts when something waits for it, also when the handler is a
        // resolving function of another promise.
        async resultOfThenWithResolvingFunctions(promise) {
            const { resolve, reject } = Promise.withResolvers();
            await promise.then(resolve, reject);
        },
        async olderOfTwoReactions(promise) {
            const result = promise.then(v => v);
            promise.catch(() => { });
            await result;
        },
    };
    checkNativeShapes = state => {
        for (const [name, shape] of Object.entries(nativeShapes)) {
            async function caller(promise) { await shape(promise); }
            const pending = new Promise(() => { });
            caller(pending);
            drainMicrotasks();
            const asyncFrames = $vm.asyncStackTraceForNativeRejection(pending).split("\n").filter(line => line).map(line => line.split("@")[0]).join(", ");
            shouldBe(asyncFrames, name === "olderOfTwoReactions" ? "" : `async ${name}, async caller`, `native rejection: ${name}, ${state}`);
        }
    };
}

// A reaction has another layout when an async context is set (AsyncLocalStorage in Bun): the context is in the
// slot that otherwise holds the promise or the cell of the job.
checkShapes(2, "no async context");
checkNativeShapes("no async context");
for (const asyncContext of [{ storage: 1 }, 42]) {
    $vm.setAsyncContext(asyncContext);
    checkShapes(1, `async context ${JSON.stringify(asyncContext)}`);
    checkNativeShapes(`async context ${JSON.stringify(asyncContext)}`);
}
$vm.setAsyncContext(undefined);

// The body of an async generator runs at once inside next(), `for await` and `yield*`. That VM entry is no job:
// what called it is on the stack, and the async frames are those of the job that is on the stack, if there is one
// (async-stack-trace-promise-reaction-job.js has the shapes whose job is a handler).
{
    const syncRunShapes = {
        async yieldStarFirstRun() {
            async function* inner() { thrower(); }
            async function* outer() { yield* inner(); }
            for await (const value of outer()) { }
        },
        async nextFirstRun() {
            async function* generator() { thrower(); }
            await generator().next();
        },
        // The job is the body of an async generator that a microtask resumed.
        async resumedGeneratorCallsNext() {
            async function* inner() { thrower(); }
            async function* outer() { await 0; const promise = inner().next(); await promise; }
            for await (const value of outer()) { }
        },
        async resumedGeneratorForAwait() {
            async function* inner() { thrower(); }
            async function* outer() { await 0; for await (const value of inner()) { } }
            for await (const value of outer()) { }
        },
    };
    const expectedFrames = {
        yieldStarFirstRun: "thrower, inner, outer, yieldStarFirstRun, caller",
        nextFirstRun: "thrower, generator, next, nextFirstRun, caller",
        resumedGeneratorCallsNext: "thrower, inner, next, outer, async resumedGeneratorCallsNext, async caller",
        resumedGeneratorForAwait: "thrower, inner, outer, async resumedGeneratorForAwait, async caller",
    };
    for (const [name, shape] of Object.entries(syncRunShapes)) {
        async function caller() { await shape(); }
        let error;
        // Not from global code: its frame would be the last one of the shapes that are all on the stack.
        (function start() { caller().then(() => { throw new Error(`${name} did not throw`); }, e => { error = e; }); })();
        drainMicrotasks();
        shouldBe(framesOf(error).replace(/, start, global code$/, ""), expectedFrames[name], name);
    }
}

// A resolve function that was called settles nothing more, so it is no link. Each shape resolves its promise with a
// promise that stays pending. The async function that awaits it does not wait for capture().
{
    let stack;
    async function capture() {
        for (let i = 0; i < 10; i++)
            await 0;
        stack = new Error("capture").stack;
    }
    const forever = new Promise(() => { });
    const calledResolveFunctions = {
        ofWithResolvers() {
            const { promise, resolve } = Promise.withResolvers();
            resolve(forever);
            capture().then(resolve);
            return promise;
        },
        ofConstructor() {
            return new Promise(resolve => {
                resolve(forever);
                capture().then(resolve);
            });
        },
        ofAwaitedThenable() {
            return { then(resolve) { resolve(forever); capture().then(resolve); } };
        },
        ofThenableInResolve() {
            return Promise.resolve({ then(resolve) { resolve(forever); capture().then(resolve); } });
        },
    };
    for (const [name, shape] of Object.entries(calledResolveFunctions)) {
        async function waiter() { await shape(); }
        stack = undefined;
        waiter();
        drainMicrotasks();
        shouldBe(stack.split("\n").filter(line => line.startsWith("async ")).join(", "), "", `the resolve function ${name} after it was called`);
    }
}

// A link from a promise to the promise that it settles adds no frame, so Error.stackTraceLimit does not bound the
// links. The walk follows 64 links from one async function to the next. Every link without a frame counts: a then(),
// catch() or finally(), and a promise that is resolved with another (`return promise` in an async function).
{
    function chained(count) {
        return async function chain() {
            let promise = failing();
            for (let i = 0; i < count; i++)
                promise = promise.then(v => v);
            await promise;
        };
    }
    function returned(count) {
        async function level(depth) { return depth ? level(depth - 1) : failing(); }
        return async function chain() { await level(count - 1); };
    }
    function asyncFramesUnder(shape) {
        let error;
        shape().catch(e => { error = e; });
        drainMicrotasks();
        return error.stack.split("\n").filter(line => line.startsWith("async ")).map(line => line.split("@")[0]).join(", ");
    }
    shouldBe(asyncFramesUnder(chained(63)), "async chain", "63 then() links");
    shouldBe(asyncFramesUnder(chained(64)), "", "64 then() links");
    shouldBe(asyncFramesUnder(returned(63)), "async chain", "63 async functions that return the promise of the next");
    shouldBe(asyncFramesUnder(returned(64)), "", "64 async functions that return the promise of the next");
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

// An await cycle with no combinator in it, whose async functions add frames: a() awaits a promise that b() settles
// through a resolve function, and b() awaits a(). With a limit the trace has at most that many frames. Without one
// the walk ends too.
{
    let stack;
    async function leaf() {
        for (let i = 0; i < 10; i++)
            await 0;
        stack = new Error("cycle").stack;
    }
    async function c() { await leaf(); }
    function asyncFramesInCycle() {
        let promiseOfA, promiseOfB;
        async function a() {
            await 0;
            const { promise, resolve, reject } = Promise.withResolvers();
            promiseOfB.then(resolve, reject);
            c().then(resolve, reject);
            await promise;
        }
        async function b() { await promiseOfA; }
        promiseOfA = a();
        promiseOfB = b();
        drainMicrotasks();
        return stack.split("\n").filter(line => line.startsWith("async ")).map(line => line.split("@")[0]);
    }
    Error.stackTraceLimit = 10;
    let frames = asyncFramesInCycle();
    shouldBe(frames.slice(0, 3).join(", "), "async c, async a, async b", "cycle with frames, limit 10");
    shouldBe(frames.length <= 10, true, "cycle with frames, limit 10");
    Error.stackTraceLimit = Infinity;
    frames = asyncFramesInCycle();
    shouldBe(frames.slice(0, 3).join(", "), "async c, async a, async b", "cycle with frames, no limit");
    Error.stackTraceLimit = 100;
}

// then() is no longer the function that the realm began with, and the prototype is frozen. The species is still the
// Promise of the realm, so then() stores the promise that it returns, and every shape has the frames it had.
{
    const originalThen = Promise.prototype.then;
    Promise.prototype.then = function then(onFulfilled, onRejected) { return originalThen.call(this, onFulfilled, onRejected); };
    checkShapes(1, "then() replaced");
    checkNativeShapes("then() replaced");
    Promise.prototype.then = originalThen;
    Promise.prototype.constructor = Promise;
    checkShapes(1, "constructor written");
    Object.freeze(Promise.prototype);
    checkShapes(1, "prototype frozen");
    checkNativeShapes("prototype frozen");
}
