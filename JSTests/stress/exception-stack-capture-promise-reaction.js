// A throw that leaves a handler of then(), catch() or finally() captures no Exception stack when the job that
// called the handler settles a promise: the job rejects the promise with the thrown value and drops the Exception.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function thrower(value, shouldThrow) {
    if (shouldThrow)
        throw value;
}
noInline(thrower);

// Throwing on every iteration would keep the callers in baseline, since a DFG or FTL catch is an OSR exit.
function shouldThrowAt(i) {
    return !(i % 16);
}

function test(name, run, expected) {
    for (let i = 0; i < testLoopCount; ++i) {
        let value = { };
        let promise = run(value, shouldThrowAt(i));
        drainMicrotasks();
        if (shouldThrowAt(i))
            shouldBe($vm.lastExceptionHasCapturedStack(value), expected, `${name} #${i}`);
        promise.catch(() => { });
        drainMicrotasks();
    }
}

test("then handler", (value, shouldThrow) => Promise.resolve().then(() => thrower(value, shouldThrow)), false);
test("catch handler", (value, shouldThrow) => Promise.reject(0).catch(() => thrower(value, shouldThrow)), false);
test("finally handler", (value, shouldThrow) => Promise.resolve().finally(() => thrower(value, shouldThrow)), false);
test("then handler with both handlers", (value, shouldThrow) => Promise.resolve().then(() => thrower(value, shouldThrow), () => { }), false);
test("then handler of a promise that was pending", (value, shouldThrow) => {
    const { promise, resolve } = Promise.withResolvers();
    const result = promise.then(() => thrower(value, shouldThrow));
    resolve();
    return result;
}, false);
