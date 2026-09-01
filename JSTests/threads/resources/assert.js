// Shared assertion helpers for JSTests/threads tests, modeled on the
// JSTests/stress conventions. Load with:
//   load("./resources/assert.js", "caller relative");
// or from a subdirectory:
//   load("../resources/assert.js", "caller relative");

function describe(value) {
    if (typeof value === "string")
        return '"' + value + '"';
    if (value !== value)
        return "NaN";
    if (value === 0 && 1 / value < 0)
        return "-0";
    try {
        return String(value);
    } catch {
        return "<unprintable>";
    }
}

function shouldBe(actual, expected, message) {
    let equal;
    if (typeof expected === "number" && expected !== expected)
        equal = actual !== actual;
    else if (expected === 0)
        equal = actual === 0 && (1 / actual) === (1 / expected);
    else
        equal = actual === expected;
    if (!equal)
        throw new Error((message ? message + ": " : "") + "expected " + describe(expected) + " but got " + describe(actual));
}

function shouldBeTrue(actual, message) {
    shouldBe(actual, true, message);
}

function shouldBeFalse(actual, message) {
    shouldBe(actual, false, message);
}

function shouldNotThrow(func, message) {
    try {
        return func();
    } catch (error) {
        throw new Error((message ? message + ": " : "") + "expected no exception but got " + describe(error));
    }
}

// shouldThrow(fn) - any exception.
// shouldThrow(TypeError, fn) - exception must be an instance of the given
//   constructor (or have a matching constructor name, so it works across
//   realms and for ConcurrentAccessError).
// shouldThrow(fn, "message") / shouldThrow(TypeError, fn, "message") - the
//   stringified exception must equal the given string when it starts with
//   the error name, otherwise the error message must equal it.
function shouldThrow(typeOrFunc, funcOrString, expectedString) {
    let expectedType = null;
    let func = typeOrFunc;
    if (typeof funcOrString === "function" || (typeof typeOrFunc === "function" && /^(class|function)\s*[A-Z]/.test(String(typeOrFunc)))) {
        expectedType = typeOrFunc;
        func = funcOrString;
    } else
        expectedString = funcOrString;

    let threw = false;
    let error;
    try {
        func();
    } catch (caught) {
        threw = true;
        error = caught;
    }
    if (!threw)
        throw new Error("expected an exception but none was thrown");
    if (expectedType) {
        const matchesInstance = error instanceof expectedType;
        const matchesName = error && error.constructor && error.constructor.name === expectedType.name;
        const matchesErrorName = error && error.name === expectedType.name;
        if (!matchesInstance && !matchesName && !matchesErrorName)
            throw new Error("expected an exception of type " + expectedType.name + " but got " + describe(error));
    }
    if (expectedString !== undefined) {
        const actualString = String(error);
        if (actualString !== expectedString && (!error || error.message !== expectedString))
            throw new Error("expected exception " + describe(expectedString) + " but got " + describe(actualString));
    }
    return error;
}

// Spawns n Threads running fn(index) and returns the array of Thread objects.
function spawnN(n, fn) {
    const threads = [];
    for (let i = 0; i < n; ++i)
        threads.push(new Thread(fn, i));
    return threads;
}

function joinAll(threads) {
    return threads.map(thread => thread.join());
}

// Fails (throws) if fn does not complete within ms milliseconds of wall time.
// Under the phase-1 GIL this is a coarse watchdog, not a scheduler.
function withTimeout(ms, fn) {
    const start = Date.now();
    const result = fn();
    if (Date.now() - start > ms)
        throw new Error("operation exceeded timeout of " + ms + "ms");
    return result;
}
