// A throw that leaves an async function which a microtask job resumed captures no Exception stack:
// the job rejects the function's promise with the thrown value and drops the Exception.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function thrower(value, shouldThrow) {
    if (shouldThrow)
        throw value;
}
noInline(thrower);

async function throwAfterAwait(value, shouldThrow) {
    await 0;
    if (shouldThrow)
        throw value;
}

async function throwInCalleeAfterAwait(value, shouldThrow) {
    await 0;
    thrower(value, shouldThrow);
}

async function throwInFinallyBodyAfterAwait(value, shouldThrow) {
    await 0;
    try {
        thrower(value, shouldThrow);
    } finally { }
}

async function rejectedAwait(value, shouldThrow) {
    await (shouldThrow ? Promise.reject(value) : 0);
}

async function rethrowFromInnerAsyncFunction(value, shouldThrow) {
    await throwAfterAwait(value, shouldThrow);
}

// Still captured: the throw leaves another VM entry first, and native code sees the Exception.
async function throwInNativeCallbackAfterAwait(value, shouldThrow) {
    await 0;
    JSON.parse("1", () => thrower(value, shouldThrow));
}

// Still captured: the first run of an async function is a call from its caller, not a microtask job.
async function throwBeforeAwait(value, shouldThrow) {
    if (shouldThrow)
        throw value;
    await 0;
}

// A user catch clause takes the value, as it does in a function that is not async.
async function userCatchAfterAwait(value, shouldThrow) {
    await 0;
    try {
        thrower(value, shouldThrow);
    } catch { }
}

// Throwing on every iteration would keep the callers in baseline, since a DFG or FTL catch is an OSR exit.
function shouldThrowAt(i) {
    return !(i % 16);
}

// The throw of throwBeforeAwait happens in the call. The others throw in a microtask job.
function test(name, run, expected, throwsInCall = false) {
    for (let i = 0; i < testLoopCount; ++i) {
        let value = { };
        let promise = run(value, shouldThrowAt(i));
        if (!throwsInCall)
            drainMicrotasks();
        if (shouldThrowAt(i))
            shouldBe($vm.lastExceptionHasCapturedStack(value), expected, `${name} #${i}`);
        promise.catch(() => { });
        drainMicrotasks();
    }
}

test("throw after await", throwAfterAwait, false);
test("throw in callee after await", throwInCalleeAfterAwait, false);
test("throw in try/finally after await", throwInFinallyBodyAfterAwait, false);
test("rejected await", rejectedAwait, false);
test("rethrow from inner async function", rethrowFromInnerAsyncFunction, false);
test("user catch after await", userCatchAfterAwait, false);
test("throw in native callback after await", throwInNativeCallbackAfterAwait, true);
test("throw before await", throwBeforeAwait, true, true);
