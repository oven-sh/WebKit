//@ requireOptions("--useSoundTypes=0")

// With the option off (the default), $$t is an identifier like any other.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

let error = null;
try {
    $$t(1, 8);
} catch (e) {
    error = e;
}
shouldBe(error instanceof ReferenceError, true);

(function () {
    let calls = 0;
    function $$t(value, mask) {
        calls++;
        return mask;
    }
    noInline($$t);
    function f(a) {
        $$t(a, 8);
        return $$t(a, 16);
    }
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(f("s"), 16);
    shouldBe(calls, testLoopCount * 2);
})();
