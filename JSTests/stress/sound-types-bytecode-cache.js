//@ runBytecodeCache("--useSoundTypes=1")

// op_check_type survives a round trip through the bytecode cache, in the program and in functions compiled later.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

function messageOf(func) {
    try {
        func();
    } catch (e) {
        return e.message;
    }
    return "did not throw";
}

shouldBe($$t(1, 8), 1);
shouldBe(messageOf(() => $$t(1, 1008)), "Type check failed: expected string | symbol | bigint | object, got number");

function outer(a) {
    $$t(a, 8);
    return function inner(b) {
        $$t(b, 16 | 1);
        return $$t(String(a) + b, 16);
    };
}

shouldBe(outer(1)("s"), "1s");
shouldBe(outer(1)(), "1undefined");
shouldBe(messageOf(() => outer("s")), "Type check failed: expected number, got string");
shouldBe(messageOf(() => outer(1)(null)), "Type check failed: expected string | undefined, got null");

var program = `(function (a) { $$t(a, 256); return a.length; })`;
shouldBe(loadString(program)([1]), 1);
shouldBe(messageOf(() => loadString(program)("s")), "Type check failed: expected array, got string");
