// A throw that leaves a handler of then(), catch() or finally() captures no Exception stack when the job that
// called the handler settles a promise: the job rejects the promise with the thrown value and drops the Exception.
// The same holds for a handler that is a bound function.

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

// The target of a bound function runs in the VM entry of the job.
function throwerOfThis() {
    if (this.shouldThrow)
        throw this.value;
}
noInline(throwerOfThis);
test("bound then handler", (value, shouldThrow) => Promise.resolve().then(throwerOfThis.bind({ value, shouldThrow })), false);
test("bound then handler with bound arguments", (value, shouldThrow) => Promise.resolve().then(thrower.bind(null, value, shouldThrow)), false);
test("bound catch handler", (value, shouldThrow) => Promise.reject(0).catch(throwerOfThis.bind({ value, shouldThrow })), false);
test("bound finally handler", (value, shouldThrow) => Promise.resolve().finally(throwerOfThis.bind({ value, shouldThrow })), false);

// Still captured: the throw leaves another VM entry first, and native code sees the Exception.
test("throw in a native callback in a handler", (value, shouldThrow) => Promise.resolve().then(() => { JSON.parse("1", () => thrower(value, shouldThrow)); }), true);
test("handler that is a Proxy", (value, shouldThrow) => Promise.resolve().then(new Proxy(() => thrower(value, shouldThrow), { })), true);
