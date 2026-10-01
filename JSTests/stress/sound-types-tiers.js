//@ requireOptions("--useSoundTypes=1")

// Each function is warmed up until the highest enabled tier has compiled it, and only then fails its check. In the DFG and
// FTL a failure is an OSR exit to op_check_type, which throws from the baseline tier.

function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

function messageOf(func) {
    try {
        func();
    } catch (e) {
        if (!(e instanceof TypeError))
            throw e;
        return e.message;
    }
    throw new Error("did not throw");
}

// Warm up, fail, and keep going: the function must still work after the exit, and after being recompiled.
function exercise(func, good, bad, message) {
    noInline(func);
    for (let round = 0; round < 3; ++round) {
        for (let i = 0; i < testLoopCount; ++i) {
            let [args, expected] = good[i % good.length];
            shouldBe(func(...args), expected);
        }
        for (let args of bad)
            shouldBe(messageOf(() => func(...args)), message);
    }
}

// Masks that Fixup turns into Check(child: SomeUse).
exercise(function (a, b) { $$t(a, 8); $$t(b, 8); return a + b; },
    [[[1, 2], 3]], [["1", 2], ["", 2]], "Type check failed: expected number, got string");
exercise(function (a, b) { $$t(a, 8); $$t(b, 8); return a + b; },
    [[[1.5, 2], 3.5], [[1, 2], 3], [[0x7fffffff, 1], 0x80000000]], [[1, "2"]], "Type check failed: expected number, got string");
exercise(function (a) { $$t(a, 16); return a.length; }, [[["abc"], 3], [[""], 0]], [[3]], "Type check failed: expected string, got number");
exercise(function (a) { $$t(a, 4); return a ? 1 : 2; }, [[[true], 1], [[false], 2]], [[0]], "Type check failed: expected boolean, got number");
exercise(function (a) { $$t(a, 32); return a.description; }, [[[Symbol("d")], "d"]], [["d"]], "Type check failed: expected symbol, got string");
exercise(function (a) { $$t(a, 64); return a + 1n; }, [[[1n], 2n], [[2n ** 64n], 2n ** 64n + 1n]], [[1]], "Type check failed: expected bigint, got number");
exercise(function (a) { $$t(a, 3); return a == null; }, [[[null], true], [[undefined], true]], [[0]], "Type check failed: expected null | undefined, got number");
exercise(function (a) { $$t(a, 7); return !a; }, [[[null], true], [[undefined], true], [[true], false]], [[0]], "Type check failed: expected boolean | null | undefined, got number");
exercise(function (a) { $$t(a, 15); return a ?? 1; }, [[[null], 1], [[2], 2], [[true], true], [[2.5], 2.5]], [["s"]], "Type check failed: expected boolean | number | null | undefined, got string");
exercise(function (a) { $$t(a, 19); return a ?? "d"; }, [[[null], "d"], [["s"], "s"]], [[1]], "Type check failed: expected string | null | undefined, got number");
exercise(function (a) { $$t(a, 896); return typeof a; }, [[[{ }], "object"], [[[]], "object"], [[() => { }], "function"]], [["s"]], "Type check failed: expected object, got string");
exercise(function (a) { $$t(a, 899); return a?.x; }, [[[{ x: 1 }], 1], [[null], undefined]], [[1]], "Type check failed: expected object | null | undefined, got number");
exercise(function (a) { $$t(a, 1008); return typeof a; }, [[[{ }], "object"], [["s"], "string"], [[1n], "bigint"]], [[1]], "Type check failed: expected string | symbol | bigint | object, got number");
exercise(function (a) { $$t(a, 1011); return typeof a; }, [[[{ }], "object"], [["s"], "string"], [[undefined], "undefined"]], [[1]], "Type check failed: expected string | symbol | bigint | object | null | undefined, got number");
exercise(function (a) { $$t(a, 991); return typeof a; }, [[[{ }], "object"], [["s"], "string"], [[1], "number"]], [[Symbol()]], "Type check failed: expected boolean | number | string | bigint | object | null | undefined, got symbol");

// Masks that stay CheckSoundType.
exercise(function (a) { $$t(a, 9); return a === undefined ? 0 : a + 1; }, [[[1], 2], [[undefined], 0], [[1.5], 2.5]], [[null]], "Type check failed: expected number | undefined, got null");
exercise(function (a) { $$t(a, 1); return a; }, [[[undefined], undefined]], [[null]], "Type check failed: expected undefined, got null");
exercise(function (a) { $$t(a, 2); return a; }, [[[null], null]], [[undefined]], "Type check failed: expected null, got undefined");
exercise(function (a) { $$t(a, 24); return a + a; }, [[[1], 2], [["a"], "aa"]], [[true]], "Type check failed: expected number | string, got boolean");
exercise(function (a) { $$t(a, 256); return a.length; }, [[[[1]], 1], [[[]], 0], [[new (class extends Array { })], 0]], [[{ length: 1 }]], "Type check failed: expected array, got object");
exercise(function (a) { $$t(a, 256); return a.length; }, [[[[1]], 1]], [[new Proxy([], { })]], "Type check failed: expected array, got object");
exercise(function (a) { $$t(a, 128); return a(); }, [[[() => 1], 1]], [[{ }]], "Type check failed: expected function, got object");
exercise(function (a) { $$t(a, 128); return typeof a; }, [[[() => 1], "function"], [[Array], "function"], [[new Proxy(() => 1, { })], "function"]], [[new Proxy({ }, { })]], "Type check failed: expected function, got object");
exercise(function (a) { $$t(a, 512); return a.x; }, [[[{ x: 1 }], 1]], [[[]]], "Type check failed: expected object, got array");
exercise(function (a) { $$t(a, 512); return a.x; }, [[[{ x: 1 }], 1], [[new Proxy({ x: 2 }, { })], 2], [[new Map], undefined]], [[new Proxy(() => 1, { })]], "Type check failed: expected object, got function");
exercise(function (a) { $$t(a, 512); return a.x; }, [[[{ x: 1 }], 1]], [[Array]], "Type check failed: expected object, got function");
exercise(function (a) { $$t(a, 640); return typeof a; }, [[[{ }], "object"], [[() => 1], "function"]], [[[]]], "Type check failed: expected function | object, got array");
exercise(function (a) { $$t(a, 768); return typeof a; }, [[[{ }], "object"], [[[]], "object"]], [[() => 1]], "Type check failed: expected array | object, got function");
exercise(function (a) { $$t(a, 384); return typeof a; }, [[[() => 1], "function"], [[[]], "object"]], [[{ }]], "Type check failed: expected function | array, got object");
exercise(function (a) { $$t(a, 514); return a === null ? 0 : a.x; }, [[[{ x: 1 }], 1], [[null], 0]], [[undefined]], "Type check failed: expected object | null, got undefined");
exercise(function (a) { $$t(a, 127); return typeof a; }, [[[1], "number"], [["s"], "string"], [[null], "object"]], [[{ }]], "Type check failed: expected boolean | number | string | symbol | bigint | null | undefined, got object");

// try / catch in the optimized function itself.
(function () {
    function f(a) {
        let before = a;
        try {
            $$t(a, 8);
            return a + 1;
        } catch (e) {
            return e.message + " " + before;
        }
    }
    noInline(f);
    for (let round = 0; round < 3; ++round) {
        for (let i = 0; i < testLoopCount; ++i)
            shouldBe(f(i), i + 1);
        shouldBe(f("s"), "Type check failed: expected number, got string s");
    }
})();

// The failing function is inlined, and the handler is in the caller.
(function () {
    function callee(a) {
        $$t(a, 8);
        return a * 2;
    }
    function caller(a, b) {
        let sum = 0;
        try {
            sum += callee(a);
            sum += callee(b);
        } catch (e) {
            return sum + ":" + e.message;
        }
        return sum;
    }
    noInline(caller);
    for (let round = 0; round < 3; ++round) {
        for (let i = 0; i < testLoopCount; ++i)
            shouldBe(caller(i, 1), i * 2 + 2);
        shouldBe(caller(1, "s"), "2:Type check failed: expected number, got string");
        shouldBe(caller(null, 1), "0:Type check failed: expected number, got null");
    }
})();

// The failure happens in the middle of a hot loop, entered by OSR.
(function () {
    function f(array) {
        let sum = 0;
        for (let i = 0; i < array.length; ++i) {
            let v = array[i];
            $$t(v, 8);
            sum += v;
        }
        return sum;
    }
    noInline(f);
    let array = [];
    for (let i = 0; i < testLoopCount * 10; ++i)
        array.push(i & 15);
    let expected = f(array);
    for (let i = 0; i < 5; ++i)
        shouldBe(f(array), expected);
    array[array.length - 2] = "s";
    shouldBe(messageOf(() => f(array)), "Type check failed: expected number, got string");
})();

// A loop-invariant check that is not executed on every path must not fail on behalf of the paths that skip it.
(function () {
    function f(v, n, doCheck) {
        let sum = 0;
        for (let i = 0; i < n; ++i) {
            if (doCheck) {
                $$t(v, 8);
                sum += v;
            } else
                sum++;
        }
        return sum;
    }
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(f(2, 10, true), 20);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(f("s", 10, false), 10);
        shouldBe(f("s", 0, true), 0);
    }
    shouldBe(messageOf(() => f("s", 10, true)), "Type check failed: expected number, got string");
})();

// Side effects before the failing check are kept, and are not replayed by the exit.
(function () {
    let o = { count: 0 };
    function f(o, v) {
        o.count++;
        $$t(v, 16);
        o.count += 10;
        return v;
    }
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        f(o, "s");
    shouldBe(o.count, testLoopCount * 11);
    messageOf(() => f(o, 1));
    shouldBe(o.count, testLoopCount * 11 + 1);
})();

// Constants, both passing and failing.
(function () {
    function pass() { return $$t(1, 8) + $$t(undefined, 9) + $$t("s", 16) + $$t(null, 2); }
    function failNumber() { return $$t(1, 16); }
    function failNull() { return $$t(null, 9); }
    function failUndefined() { return $$t(undefined, 10); }
    for (let f of [pass, failNumber, failNull, failUndefined])
        noInline(f);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(pass(), "NaNsnull");
        shouldBe(messageOf(failNumber), "Type check failed: expected string, got number");
        shouldBe(messageOf(failNull), "Type check failed: expected number | undefined, got null");
        shouldBe(messageOf(failUndefined), "Type check failed: expected number | null, got undefined");
    }
})();

// A function that fails as often as it passes stays correct however often it is recompiled.
(function () {
    function f(a) {
        $$t(a, 8);
        return a + 1;
    }
    noInline(f);
    let failures = 0;
    for (let i = 0; i < testLoopCount * 4; ++i) {
        try {
            shouldBe(f(i & 1 ? i : "s"), i + 1);
        } catch (e) {
            shouldBe(e.message, "Type check failed: expected number, got string");
            failures++;
        }
    }
    shouldBe(failures, testLoopCount * 2);
})();

// Values produced in unboxed representations by the DFG and FTL.
(function () {
    function f(a, b) {
        let d = $$t(a * 1.5, 8);
        let i = $$t(a | 0, 8);
        let big = $$t(a * 4294967296 + b, 8);
        let c = $$t(a < b, 4);
        let s = $$t(a + "", 16);
        return $$t(d + i + big, 8) + (c ? 1 : 0) + s.length;
    }
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(f(2, 3), 3 + 2 + 8589934595 + 1 + 1);
})();

// Allocations that the FTL would like to sink.
(function () {
    function f(x) {
        let o = $$t({ x }, 512);
        let a = $$t([x], 256);
        let g = $$t(() => x, 128);
        return o.x + a[0] + g();
    }
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(f(i), i * 3);
})();
