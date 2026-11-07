//@ defaultRun; run("debugger-bytecode", "--forceDebuggerBytecodeGeneration=true")

// A builtin whose source has a URL is one of the embedder's modules. It has expression info in every build, and an
// error raised in it quotes its source text like an error in user code does. The text must be that of the expression
// that failed. It was looked up at bytecode index 0: the first expression of the function (`object` and `callback`
// below), or the head of the function when the bytecode has debugger hooks.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function messageOf(callback) {
    try {
        callback();
    } catch (error) {
        return error.message;
    }
    return "did not throw";
}

const readValueSource = `(function (object) {
    var copy = object;
    return copy.state.value;
})`;

const readBeforeInitializationSource = `(function (callback, ready) {
    callback();
    if (ready)
        return ready;
    var before = later;
    let later = 1;
    return before;
})`;

const nothing = () => { };

function test(readValue, readBeforeInitialization, iterations, message) {
    for (let i = 0; i < iterations; i++) {
        // Most calls succeed, so that the builtins tier up. The first call and every 16th call after it fail.
        if (i % 16) {
            shouldBe(readValue({ state: { value: i } }), i, message);
            shouldBe(readBeforeInitialization(nothing, true), true, message);
            continue;
        }
        shouldBe(messageOf(() => readValue({ })), "undefined is not an object (evaluating 'copy.state.value')", message);
        shouldBe(messageOf(() => readBeforeInitialization(nothing, false)), "Cannot access 'later' before initialization.", message);
    }
}

// builtinFromBytecodeCache() makes a builtin with a source URL, the way an embedder does: from source, or from the
// bytes that encodeBuiltinFunction() made of it.
test(builtinFromBytecodeCache(readValueSource, false), builtinFromBytecodeCache(readBeforeInitializationSource, false), testLoopCount, "from source");
test(builtinFromBytecodeCache(readValueSource, true), builtinFromBytecodeCache(readBeforeInitializationSource, true), 32, "from the bytecode cache");

// $vm.createBuiltin() gives its source no URL: a private builtin, like the engine's own. appendSourceToErrorMessage()
// keeps its text out of the message in every build.
const privateReadValue = $vm.createBuiltin(readValueSource);
for (let i = 0; i < 32; i++)
    shouldBe(messageOf(() => privateReadValue({ })), "undefined is not an object", "private builtin");

// Each way to fail below gives the message that the same function gives as user code. The expression that fails is
// never the first one of the function. In the `Object.keys` body a host function makes the error, and the text is
// that of the call.
const bodies = [
    "return copy.state.value;",
    "return copy.state[key];",
    "copy.state.value = 1;",
    "delete copy.state.value;",
    "return key in copy.state;",
    "return copy instanceof copy.state;",
    "return new copy.state();",
    "return copy.state();",
    "return Object.keys(copy.state);",
    "var before = later; let later = copy; return before;",
];
for (const body of bodies) {
    const source = `(function (object, key) {
    var copy = object;
    ${body}
})`;
    const expected = messageOf(() => (0, eval)(source)({ }, "key"));
    shouldBe(expected === "did not throw", false, body);
    for (const roundTrip of [false, true]) {
        const builtin = builtinFromBytecodeCache(source, roundTrip);
        for (let i = 0; i < 3; i++)
            shouldBe(messageOf(() => builtin({ }, "key")), expected, body);
    }
}
shouldBe(messageOf(() => builtinFromBytecodeCache(`(function (object) {
    var copy = object;
    return Object.keys(copy.state);
})`, false)({ })), "undefined is not an object (evaluating 'Object.keys(copy.state)')", "host function");
