//@ requireOptions("--useAsyncStackTrace=1")

// An Error that a handler of then(), catch() or finally() makes has the async frames of what waits for the promise
// that the call returned: the job of the handler settles that promise, and no async function is on the stack.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function thrower() {
    throw new Error("thrower");
}

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

// The function that made the Error, and the async frames. A handler of finally() has a frame of JSC between them
// when then() is not the function that the realm began with.
function topAndAsyncFramesOf(error) {
    const frames = framesOf(error).split(", ");
    return [frames[0], ...frames.filter(frame => frame.startsWith("async "))].join(", ");
}

const shapes = {
    async handlerThrows() { await Promise.resolve(1).then(thrower); },
    async catchHandlerThrows() { await Promise.reject(1).catch(thrower); },
    async finallyHandlerThrows() { await Promise.resolve(1).finally(thrower); },
    async rejectHandlerOfThenThrows() { await Promise.reject(1).then(undefined, thrower); },
    async handlerOfPendingPromiseThrows() {
        const { promise, resolve } = Promise.withResolvers();
        const result = promise.then(thrower);
        resolve();
        await result;
    },
    // The target of a bound function runs in a VM entry of its own.
    async boundHandlerThrows() { await Promise.resolve(1).then(thrower.bind(null)); },
};

// The walk goes through one combinator while then() is the function that the realm began with.
const shapesWithCombinator = {
    async handlerThrowsInAll() { await Promise.all([Promise.resolve(1).then(thrower)]); },
    async handlerThrowsInRace() { await Promise.race([Promise.resolve(1).then(thrower)]); },
};

// The job is found also when no frame of its VM entry is in the trace: the handler is the Error constructor, calls
// it in a tail call, or takes itself out with Error.captureStackTrace.
const shapesWithoutFrameOfTheJob = {
    async handlerReturnsErrorInTailCall() { throw await Promise.resolve(1).then(function handler() { "use strict"; return Error("x"); }); },
    async handlerIsErrorConstructor() { throw await Promise.resolve("x").then(Error); },
    async handlerCapturesFromItself() {
        throw await Promise.resolve(1).then(function handler() {
            const error = new Error("x");
            Error.captureStackTrace(error, handler);
            return error;
        });
    },
};

function checkShapes(loopCount, state, shapes) {
    for (const [name, shape] of Object.entries(shapes)) {
        async function caller() { await shape(); }
        for (let i = 0; i < loopCount; i++) {
            let error;
            caller().then(() => { throw new Error(`${name} did not throw`); }, e => { error = e; });
            drainMicrotasks();
            shouldBe(topAndAsyncFramesOf(error), `thrower, async ${name}, async caller`, `${name}, ${state}`);
        }
    }
    for (const [name, shape] of Object.entries(shapesWithoutFrameOfTheJob)) {
        async function caller() { await shape(); }
        let error;
        caller().then(() => { throw new Error(`${name} did not throw`); }, e => { error = e; });
        drainMicrotasks();
        shouldBe(framesOf(error), `async ${name}, async caller`, `${name}, ${state}`);
    }
}

checkShapes(Math.min(testLoopCount, 20), "no async context", { ...shapes, ...shapesWithCombinator });
// A reaction has another layout when an async context is set (AsyncLocalStorage in Bun).
for (const asyncContext of [{ storage: 1 }, 42]) {
    $vm.setAsyncContext(asyncContext);
    checkShapes(2, `async context ${JSON.stringify(asyncContext)}`, { ...shapes, ...shapesWithCombinator });
}
$vm.setAsyncContext(undefined);

// A caller that is not on the stack leaves the trace empty.
{
    let stack;
    async function callerNotOnStack() {
        await Promise.resolve(1).then(function handler() {
            const error = new Error("x");
            Error.captureStackTrace(error, function notOnStack() { });
            stack = error.stack;
        });
    }
    callerNotOnStack();
    drainMicrotasks();
    shouldBe(stack, "", "callerNotOnStack");
}

// The body of an async generator runs at once inside next() and `for await`. That VM entry is no job: what called
// it is on the stack, and the async frames are those of the job of the handler.
{
    const syncRunShapes = {
        async forAwaitFirstRun() {
            async function* generator() { thrower(); }
            for await (const value of generator()) { }
        },
        async handlerCallsNext() {
            async function* generator() { thrower(); }
            await Promise.resolve(1).then(function handler() { const promise = generator().next(); return promise; });
        },
        async handlerStartsForAwait() {
            async function* generator() { thrower(); }
            async function consume() { for await (const value of generator()) { } }
            await Promise.resolve(1).then(function handler() { const promise = consume(); return promise; });
        },
    };
    const expectedFrames = {
        forAwaitFirstRun: "thrower, generator, forAwaitFirstRun, caller",
        handlerCallsNext: "thrower, generator, next, handler, async handlerCallsNext, async caller",
        handlerStartsForAwait: "thrower, generator, consume, handler, async handlerStartsForAwait, async caller",
    };
    for (const [name, shape] of Object.entries(syncRunShapes)) {
        async function caller() { await shape(); }
        let error;
        // Not from global code: its frame would be the last one of the shape that is all on the stack.
        (function start() { caller().then(() => { throw new Error(`${name} did not throw`); }, e => { error = e; }); })();
        drainMicrotasks();
        shouldBe(framesOf(error).replace(/, start, global code$/, ""), expectedFrames[name], name);
    }
}

// A job that drains the microtask queue runs other jobs above it on the stack. Each job has its async frames where
// its own frames end.
{
    let error;
    function capture() { error = new Error("capture"); }
    async function innerWaiter() { await Promise.resolve(1).then(function innerHandler() { capture(); }); }
    async function innerResumed() { await 0; capture(); }
    async function innerResumedWaiter() { await innerResumed(); }
    const nestedJobs = {
        // A handler drains, and a handler makes the Error.
        async handlerInHandler() { await Promise.resolve(1).then(function outerHandler() { innerWaiter(); drainMicrotasks(); }); },
        // A resumed async function drains, and a handler makes the Error.
        async handlerInResumedFunction() { await 0; innerWaiter(); drainMicrotasks(); },
        // A handler drains, and a resumed async function makes the Error.
        async resumedFunctionInHandler() { await Promise.resolve(1).then(function outerHandler() { innerResumedWaiter(); drainMicrotasks(); }); },
        // A resumed async function drains, and a resumed async function makes the Error.
        async resumedFunctionInResumedFunction() { await 0; innerResumedWaiter(); drainMicrotasks(); },
        // Nothing waits for the job that makes the Error.
        async floatingHandlerInHandler() { await Promise.resolve(1).then(function outerHandler() { Promise.resolve(1).then(function floatingHandler() { capture(); }); drainMicrotasks(); }); },
    };
    const expectedFrames = {
        handlerInHandler: "capture, innerHandler, async innerWaiter, drainMicrotasks, outerHandler, async handlerInHandler, async caller",
        handlerInResumedFunction: "capture, innerHandler, async innerWaiter, drainMicrotasks, handlerInResumedFunction, async caller",
        resumedFunctionInHandler: "capture, innerResumed, async innerResumedWaiter, drainMicrotasks, outerHandler, async resumedFunctionInHandler, async caller",
        resumedFunctionInResumedFunction: "capture, innerResumed, async innerResumedWaiter, drainMicrotasks, resumedFunctionInResumedFunction, async caller",
        floatingHandlerInHandler: "capture, floatingHandler, drainMicrotasks, outerHandler, async floatingHandlerInHandler, async caller",
    };
    for (const [name, shape] of Object.entries(nestedJobs)) {
        async function caller() { await shape(); }
        error = undefined;
        caller();
        drainMicrotasks();
        // All frames down to the drainMicrotasks() of this loop.
        const frames = error.stack.split("\n").map(line => line.split("@")[0]).join(", ");
        shouldBe(frames.replace(/, drainMicrotasks, global code$/, ""), expectedFrames[name], name);
    }
}

// No limit is no limit for each job on the stack. The job that drains is an async function whose awaiters are a
// cycle: second() awaits a race of outer() and third(), and third() awaits second(). The walk ends at the cycle.
{
    let error;
    async function innerResumed() { await 0; error = new Error("capture"); }
    async function innerWaiter() { await innerResumed(); }
    let promiseOfOuter, promiseOfSecond, promiseOfThird;
    async function outer() { await 0; innerWaiter(); drainMicrotasks(); }
    async function second() { await Promise.race([promiseOfOuter, promiseOfThird]); }
    async function third() { await 0; await promiseOfSecond; }
    const savedLimit = Error.stackTraceLimit;
    Error.stackTraceLimit = Infinity;
    promiseOfOuter = outer();
    promiseOfThird = third();
    promiseOfSecond = second();
    drainMicrotasks();
    Error.stackTraceLimit = savedLimit;
    const frames = error.stack.split("\n").map(line => line.split("@")[0]).join(", ");
    shouldBe(frames.replace(/, drainMicrotasks, global code$/, ""), "innerResumed, async innerWaiter, drainMicrotasks, outer, async second, async third", "a cycle under the outer job, no limit");
}

// then() is no longer the function that the realm began with, and the prototype is frozen. The species is still the
// Promise of the realm, so then() stores the promise that it returns, and the job says which promise it settles.
{
    const originalThen = Promise.prototype.then;
    Promise.prototype.then = function then(onFulfilled, onRejected) { return originalThen.call(this, onFulfilled, onRejected); };
    checkShapes(2, "then() replaced", shapes);
    Promise.prototype.then = originalThen;
    Promise.prototype.constructor = Promise;
    checkShapes(2, "constructor written", shapes);
    Object.freeze(Promise.prototype);
    checkShapes(2, "prototype frozen", shapes);
}
