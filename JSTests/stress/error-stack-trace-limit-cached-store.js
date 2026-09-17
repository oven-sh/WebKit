// ErrorConstructor::put() mirrors Error.stackTraceLimit into the global object.
// A store that a JIT tier caches or folds must still reach it.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

function leaf() { return new Error().stack; }
function c() { return leaf(); }
function b() { return c(); }
function a() { return b(); }

function frames() {
    var stack = a();
    if (stack === undefined || stack === "")
        return 0;
    return stack.split("\n").length;
}

function check(name, i, want) {
    shouldBe(Error.stackTraceLimit, want, name + ": Error.stackTraceLimit at iteration " + i);
    shouldBe(frames(), want, name + ": frames of a new Error at iteration " + i);
}

function run(name, store) {
    noInline(store);
    for (var i = 0; i < testLoopCount; ++i) {
        var want = 1 + (i % 3);
        store(want);
        // Creating an Error is slow. Check the first iterations, then a sample.
        if (i < 200 || !(i % 97))
            check(name, i, want);
    }
    for (var want = 1; want <= 3; ++want) {
        store(want);
        check(name, "final", want);
    }
}

run("put_by_id", function (n) { Error.stackTraceLimit = n; });

var key = "stackTraceLimit";
run("put_by_val", function (n) { Error[key] = n; });

// A load in the same function gives the DFG a proven structure for the base of the store.
run("load then store", function (n) {
    var previous = Error.stackTraceLimit;
    Error.stackTraceLimit = n;
    return previous;
});

run("save, zero, restore", function (n) {
    var previous = Error.stackTraceLimit;
    Error.stackTraceLimit = 0;
    var e = new Error();
    Error.stackTraceLimit = previous;
    Error.stackTraceLimit = n;
    return e;
});
