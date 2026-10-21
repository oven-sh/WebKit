//@ requireOptions("--useAsyncStackTrace=1")
// Not the two modes that compile each function on the main thread, tier by tier, while the collector runs:
// they take more than the 200ms of JSTests/README.md for a test with this many functions.
//@ $skipModes << "dfg-eager-no-cjit-validate".to_sym
//@ $skipModes << "ftl-eager-no-cjit".to_sym

// Async functions that await each other make the await chain a cycle. A stack trace taken under
// such a cycle has to end. Before, only the frame limit ended the walk over the chain: a trace with
// no limit ran out of memory, and a trace whose cycle adds no frame never returned.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + " expected: " + expected);
}

// The functions of a trace, down to the drainMicrotasks() that ran them.
function functionsOf(stack) {
    const names = stack.split("\n").map(line => line.slice(0, line.indexOf("@")));
    const end = names.indexOf("drainMicrotasks");
    return names.slice(0, end < 0 ? names.length : end).join(", ");
}

function withStackTraceLimit(limit, run) {
    const saved = Error.stackTraceLimit;
    Error.stackTraceLimit = limit;
    try {
        return run();
    } finally {
        Error.stackTraceLimit = saved;
    }
}

function newError() {
    return new Error("here").stack;
}

function captureStackTrace() {
    const object = {};
    Error.captureStackTrace(object);
    return object.stack;
}

// "f": an async function with that name, which awaits body(). A trace has its frame.
// "-": an Array.fromAsync call that awaits body(). It awaits in a builtin async function, and a
// trace has no frame for those.
const asyncFunctions = {};
function waiter(kind, name, body) {
    if (kind === "-")
        return () => Array.fromAsync({ length: 2, 0: null, get 1() { return body(); } });
    asyncFunctions[name] ??= new Function("body", `return async function ${name}() { await null; await body(); };`);
    return asyncFunctions[name](body);
}

// The trace that `capture` takes in c. `tail` and `lap` have one "f" or "-" for each function.
// The functions of `tail` are between c and the cycle: t1 awaits c(), t2 awaits t1(), and so on.
// The functions of `lap` are the cycle: f1 awaits Promise[combinator]([promise of the last f,
// last t()]), f2 awaits the promise of f1, and so on.
function traceUnderCycle({ lap, tail = "", combinator = "race", capture = newError }) {
    let stack;
    let awaited = async function c() { await null; stack = capture(); };
    for (let i = 0; i < tail.length; i++)
        awaited = waiter(tail[i], "t" + (i + 1), awaited);
    const promises = [];
    promises.push(waiter(lap[0], "f1", () => Promise[combinator]([promises[lap.length - 1], awaited()]))());
    for (let i = 1; i < lap.length; i++) {
        const previous = promises[i - 1];
        promises.push(waiter(lap[i], "f" + (i + 1), () => previous)());
    }
    drainMicrotasks();
    return functionsOf(stack);
}

// No limit: the trace ends after one lap.
withStackTraceLimit(Infinity, () => {
    shouldBe(traceUnderCycle({ lap: "f" }), "newError, c, async f1");
    shouldBe(traceUnderCycle({ lap: "ff" }), "newError, c, async f1, async f2");
    shouldBe(traceUnderCycle({ lap: "fff" }), "newError, c, async f1, async f2, async f3");
    shouldBe(traceUnderCycle({ lap: "fffff", tail: "ff" }), "newError, c, async t1, async t2, async f1, async f2, async f3, async f4, async f5");
    shouldBe(traceUnderCycle({ lap: "ff", capture: captureStackTrace }), "captureStackTrace, c, async f1, async f2");
    for (const combinator of ["all", "allSettled", "any"])
        shouldBe(traceUnderCycle({ lap: "ff", tail: "f", combinator }), "newError, c, async t1, async f1, async f2");
});

// A limit: the cycle repeats up to it, as before.
withStackTraceLimit(7, () => {
    shouldBe(traceUnderCycle({ lap: "ff" }), "newError, c, async f1, async f2, async f1, async f2, async f1, async f2, async f1");
    shouldBe(traceUnderCycle({ lap: "fff", tail: "ff" }), "newError, c, async t1, async t2, async f1, async f2, async f3, async f1, async f2");
    shouldBe(traceUnderCycle({ lap: "-f-" }), "newError, c" + ", async f2".repeat(7));
});

// A cycle that adds no frame: no limit ends the walk over it.
for (const limit of [2, 100, Infinity]) {
    withStackTraceLimit(limit, () => {
        shouldBe(traceUnderCycle({ lap: "-" }), "newError, c");
        shouldBe(traceUnderCycle({ lap: "---", tail: "f-f" }), "newError, c, async t1, async t3");
        shouldBe(traceUnderCycle({ lap: "-", capture: captureStackTrace }), "captureStackTrace, c");
    });
}

// The other two places where Array.fromAsync awaits.
const otherFromAsyncCycles = [
    function asyncIterator() {
        let p, stack, calls = 0;
        async function c() { await null; stack = newError(); return { done: true }; }
        p = Array.fromAsync({
            [Symbol.asyncIterator]() {
                return { next() { return calls++ ? Promise.race([p, c()]) : Promise.resolve({ done: false, value: 1 }); } };
            },
        });
        drainMicrotasks();
        return functionsOf(stack);
    },
    function mapFunction() {
        let p, stack;
        async function c() { await null; stack = newError(); }
        p = Array.fromAsync([1, 2], (value, index) => index ? Promise.race([p, c()]) : value);
        drainMicrotasks();
        return functionsOf(stack);
    },
];
for (const cycle of otherFromAsyncCycles) {
    shouldBe(cycle(), "newError, c");
    shouldBe(withStackTraceLimit(Infinity, cycle), "newError, c");
}

// Every cycle of one or two functions, alone and behind one function, each function with or without
// a frame. With a limit, the trace is what it was before for a cycle that adds frames: the cycle
// repeats up to the limit.
function framesOf(pattern, prefix) {
    return [...pattern].flatMap((kind, i) => kind === "f" ? ["async " + prefix + (i + 1)] : []);
}
for (const limit of [3, Infinity]) {
    withStackTraceLimit(limit, () => {
        for (const tail of ["", "f", "-"]) {
            for (const lap of ["f", "-", "ff", "f-", "-f", "--"]) {
                const frames = framesOf(tail, "t");
                const lapFrames = framesOf(lap, "f");
                if (limit === Infinity)
                    frames.push(...lapFrames);
                else if (lapFrames.length) {
                    while (frames.length < limit)
                        frames.push(...lapFrames);
                }
                shouldBe(traceUnderCycle({ lap, tail }), ["newError", "c", ...frames.slice(0, limit)].join(", "));
            }
        }
    });
}

// No limit, larger cycles and tails: each function is in the trace once.
withStackTraceLimit(Infinity, () => {
    for (const [lapLength, tailLength] of [[5, 0], [4, 3], [7, 6], [8, 1], [1, 9], [9, 6], [9, 9]]) {
        const lap = "f".repeat(lapLength);
        const tail = "f".repeat(tailLength);
        shouldBe(traceUnderCycle({ lap, tail }), ["newError", "c", ...framesOf(tail, "t"), ...framesOf(lap, "f")].join(", "));
    }
});
