//@ requireOptions("--useSoundTypes=1")

function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

function shouldThrowTypeCheck(func, message) {
    let error = null;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error("did not throw a TypeError: " + error);
    if (error.message !== message)
        throw new Error(`bad message: "${error.message}"`);
}

function shouldThrow(func, constructor) {
    let error = null;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof constructor))
        throw new Error(`did not throw a ${constructor.name}: ${error}`);
}

shouldBe(typeof $$t, "undefined");
shouldBe($$t(1, 8), 1);
shouldBe($$t("a", 16), "a");
shouldThrowTypeCheck(() => $$t("a", 8), "Type check failed: expected number, got string");
shouldThrowTypeCheck(() => $$t("a", 9), "Type check failed: expected number | undefined, got string");
shouldThrowTypeCheck(() => $$t(null, 9), "Type check failed: expected number | undefined, got null");
shouldThrowTypeCheck(() => $$t([], 512), "Type check failed: expected object, got array");
shouldThrowTypeCheck(() => $$t(1, 896), "Type check failed: expected object, got number");
shouldThrowTypeCheck(() => $$t({ }, 384), "Type check failed: expected function | array, got object");
shouldThrowTypeCheck(() => $$t(1n, 1022 & ~64), "Type check failed: expected boolean | number | string | symbol | object | null, got bigint");

shouldThrow(() => $$t(1), ReferenceError);
shouldThrow(() => $$t(), ReferenceError);
shouldBe($$t(1, 8, 0), 1);
shouldThrow(() => $$t(1, 8, 0, 0), ReferenceError);
shouldThrow(() => $$t(1, 0), ReferenceError);
shouldThrow(() => $$t(1, 1023), ReferenceError);
shouldThrow(() => $$t(1, 1024), ReferenceError);
shouldThrow(() => $$t(1, -8), ReferenceError);
shouldThrow(() => $$t(1, 8.5), ReferenceError);
shouldThrow(() => $$t(1, 8.0), ReferenceError);
shouldThrow(() => $$t(1, 8n), ReferenceError);
shouldThrow(() => $$t(1, "8"), ReferenceError);
shouldThrow(() => { let mask = 8; return $$t(1, mask); }, ReferenceError);
shouldThrow(() => $$t(...[1], 8), ReferenceError);
shouldThrow(() => $$t(...[1, 8]), ReferenceError);
shouldThrow(() => $$t(1, ...[8]), ReferenceError);
shouldThrow(() => new $$t(1, 8), ReferenceError);
shouldThrow(() => $$t?.(1, 8), ReferenceError);
shouldThrow(() => $$t`a`, ReferenceError);
shouldThrow(() => $$t, ReferenceError);
shouldThrow(() => $$T(1, 8), ReferenceError);
shouldThrow(() => $t(1, 8), ReferenceError);
shouldThrow(() => $$tt(1, 8), ReferenceError);

(function () {
    let calls = [];
    function $$t(...args) {
        calls.push(new.target ? ["new", ...args] : args);
        return "called";
    }
    let o = { $$t };

    shouldBe($$t("a", 16), "a");
    shouldThrowTypeCheck(() => $$t("a", 8), "Type check failed: expected number, got string");
    shouldThrowTypeCheck(() => $$t("a", 8, 1), "Type check failed: expected number, got string");
    shouldBe(calls.length, 0);

    let mask = 8;
    shouldBe($$t("a", mask), "called");
    shouldBe($$t("a"), "called");
    shouldBe($$t("a", 8, 1, 0), "called");
    shouldBe($$t("a", 0), "called");
    shouldBe($$t("a", 1023), "called");
    shouldBe($$t(..."a", 8), "called");
    shouldBe($$t?.("a", 8), "called");
    shouldBe(typeof new $$t("a", 8), "object");
    shouldBe(o.$$t("a", 8), "called");
    shouldBe(o["$$t"]("a", 8), "called");
    shouldBe($$t.call(null, "a", 8), "called");
    shouldBe((0, $$t)("a", 8), "called");
    shouldBe(JSON.stringify(calls), JSON.stringify([
        ["a", 8], ["a"], ["a", 8, 1, 0], ["a", 0], ["a", 1023], ["a", 8], ["a", 8], ["new", "a", 8], ["a", 8], ["a", 8], ["a", 8], ["a", 8],
    ]));
})();

shouldBe((function ($$t) { return $$t(1, 8); })(null), 1);
shouldBe((function () { let r = $$t(1, 8); let $$t; return r; })(), 1);
shouldBe((function () { with ({ $$t() { return "called"; } }) return $$t(1, 8); })(), 1);

shouldBe(($$t)(1, 8), 1);
shouldBe($$t(1, (8)), 1);
shouldBe($$t(1, 0x8), 1);
shouldBe($$t(undefined, 8 | 1), undefined);
shouldThrowTypeCheck(() => $$t(null, 8 | 1), "Type check failed: expected number | undefined, got null");

(function () {
    let count = 0;
    let next = () => ++count;
    shouldBe($$t(next(), 8), 1);
    $$t(next(), 8);
    shouldBe(count, 2);
    shouldThrowTypeCheck(() => $$t(next(), 16), "Type check failed: expected string, got number");
    shouldBe(count, 3);
    shouldThrow(() => $$t(missing, 8), ReferenceError);
    shouldThrow(() => { $$t(tdz, 8); let tdz = 1; }, ReferenceError);
})();

shouldBe($$t($$t(1, 8), 8 | 16), 1);
shouldBe($$t(1, 8) + $$t(2, 8), 3);
shouldBe($$t("1", 16) + $$t(2, 8), "12");
shouldBe($$t(1n, 64) + $$t(2n, 64), 3n);
shouldBe(+$$t(1.5, 8), 1.5);
shouldBe(-$$t(1, 8), -1);
shouldBe(!$$t(false, 4), true);
shouldBe($$t(true, 4) ? 1 : 2, 1);
shouldBe($$t(null, 2) ?? 5, 5);
shouldBe(typeof $$t(1, 8), "number");
shouldBe($$t([1, 2], 256).length, 2);
shouldBe($$t(x => x + 1, 128)(1), 2);
shouldBe($$t({ f() { return this.v; }, v: 3 }, 512).f(), 3);
shouldBe(`${$$t(1, 8)}`, "1");
shouldBe([...$$t([1], 256)][0], 1);
shouldBe(($$t(1, 8), $$t(2, 8)), 2);

(function () {
    let a = 1;
    shouldBe($$t(a, 8) + (a = 10), 11);
    shouldBe(a, 10);
    shouldBe($$t(a, 8) + a++ + $$t(a, 8), 31);
    let f = (x, y) => x * 100 + y;
    shouldBe(f($$t(a, 8), a = 2), 1102);
})();

(function (p) {
    let l = 1;
    var v = 2;
    const o = { x: 3 };
    let captured = 4;
    let read = () => captured;
    let s = "s";
    for (let attempt of [
        () => { l = $$t(s, 8); },
        () => { v = $$t(s, 8); },
        () => { p = $$t(s, 8); },
        () => { o.x = $$t(s, 8); },
        () => { captured = $$t(s, 8); },
        () => { l += $$t(s, 8); },
        () => { [l] = [$$t(s, 8)]; },
        () => { globalThis.soundTypesGlobal = $$t(s, 8); },
    ])
        shouldThrowTypeCheck(attempt, "Type check failed: expected number, got string");

    try { l = $$t(s, 8); } catch { }
    try { v = $$t(s, 8); } catch { }
    try { p = $$t(s, 8); } catch { }
    shouldBe(l, 1);
    shouldBe(v, 2);
    shouldBe(p, 5);
    shouldBe(o.x, 3);
    shouldBe(read(), 4);
    shouldBe("soundTypesGlobal" in globalThis, false);

    l = $$t(6, 8);
    v = $$t(l, 8);
    p = $$t(v, 8);
    shouldBe(l + v + p, 18);
})(5);

(function () {
    let read;
    try {
        read = () => x;
        let x = $$t("s", 8);
    } catch { }
    shouldThrow(read, ReferenceError);
})();

(function () {
    let log = [];
    function f(v) {
        try {
            log.push("try");
            $$t(v, 8);
            log.push("passed");
            return "returned";
        } catch (e) {
            log.push(e.message);
            return "caught";
        } finally {
            log.push("finally");
        }
    }
    shouldBe(f(1), "returned");
    shouldBe(f("s"), "caught");
    shouldBe(log.join(), "try,passed,finally,try,Type check failed: expected number, got string,finally");
})();

(function () {
    let error;
    try {
        $$t("s", 8);
    } catch (e) {
        error = e;
    }
    shouldBe(typeof error.line, "number");
    shouldBe(error.stack.split("\n").length >= 2, true);
})();

shouldBe((function () { "use strict"; return $$t(this, 8); }).call(1), 1);
shouldBe(typeof (function () { return $$t(this, 512); }).call(1), "object");
shouldBe((function () { return $$t(arguments, 512).length; })(1, 2), 2);
shouldBe((function () { return $$t(new.target, 1); })(), undefined);
(function () {
    class A { get v() { return 1; } }
    class B extends A {
        constructor() {
            shouldThrow(() => $$t(this, 512), ReferenceError);
            super();
            $$t(this, 512);
        }
        get v() { return $$t(super.v, 8) + 1; }
        field = $$t(this.v, 8);
        static s = $$t("s", 16);
    }
    shouldBe(new B().field, 2);
    shouldBe(B.s, "s");
})();

(function () {
    function f(a = $$t(1, 8), { b } = { b: $$t(2, 8) }, ...rest) {
        $$t(a, 8);
        $$t(b, 8);
        $$t(rest, 256);
        return a + b + rest.length;
    }
    shouldBe(f(), 3);
    shouldBe(f(10, { b: 20 }, 1, 2), 32);
    shouldThrowTypeCheck(() => f("a"), "Type check failed: expected number, got string");
    shouldThrowTypeCheck(() => f(1, { b: null }), "Type check failed: expected number, got null");
    function g(a = $$t("s", 8)) { }
    g(1);
    shouldThrowTypeCheck(() => g(), "Type check failed: expected number, got string");
})();

(function () {
    function outer() {
        let $$t = () => "called";
        function inner(v) {
            function innermost(w) {
                $$t(w, 16);
                return [$$t(w, 16), $$t(w)];
            }
            $$t(v, 8);
            return innermost;
        }
        return inner;
    }
    let inner = outer();
    shouldThrowTypeCheck(() => inner("s"), "Type check failed: expected number, got string");
    let innermost = inner(1);
    shouldThrowTypeCheck(() => innermost(1), "Type check failed: expected string, got number");
    shouldBe(innermost("s").join(), "s,called");
})();

(function () {
    function targets() {
        return [
            function () { $$t(1, 8) = 1; },
            function () { $$t(1, 8) += 1; },
            function () { $$t(1, 8)++; },
            function () { --$$t(1, 8); },
            function () { for ($$t(1, 8) in { a: 1 }); },
            function () { for ($$t(1, 8) of [1]); },
        ];
    }
    for (let target of targets())
        shouldThrow(target, ReferenceError);
    shouldThrowTypeCheck(function () { $$t(1, 16) = 1; }, "Type check failed: expected string, got number");
    for (let source of ["$$t(1, 8) = 1", "$$t(1, 8)++", "for ($$t(1, 8) of [1]);", "[$$t(1, 8)] = [1]", "({ a: $$t(1, 8) } = { })"]) {
        shouldThrow(() => new Function(`"use strict"; ${source}`), SyntaxError);
        shouldThrow(() => new Function(`"use strict"; function inner() { ${source} }`), SyntaxError);
    }
    shouldThrow(() => new Function("[$$t(1, 8)] = [1]"), SyntaxError);
    shouldThrow(() => new Function("function inner() { [$$t(1, 8)] = [1]; }"), SyntaxError);
})();

(function () {
    function* gen(a) {
        $$t(a, 8);
        let b = $$t(yield a, 16);
        $$t(a, 8);
        yield $$t(b, 16) + a;
        try {
            $$t(yield, 4);
        } catch (e) {
            yield e.message;
        }
        return $$t(a, 8);
    }
    let g = gen(1);
    shouldBe(g.next().value, 1);
    shouldBe(g.next("s").value, "s1");
    g.next();
    shouldBe(g.next(0).value, "Type check failed: expected boolean, got number");
    shouldBe(g.next().value, 1);

    let h = gen("s");
    shouldThrowTypeCheck(() => h.next(), "Type check failed: expected number, got string");
    shouldBe(h.next().done, true);

    let g2 = gen(1);
    g2.next();
    shouldThrowTypeCheck(() => g2.next(1), "Type check failed: expected string, got number");

    let results = [];
    async function af(a) {
        $$t(a, 8);
        let b = $$t(await a, 8);
        return $$t(await Promise.resolve(String(b)), 8);
    }
    af(1).then(() => results.push("resolved"), e => results.push(e.message));
    af("s").then(() => results.push("resolved"), e => results.push(e.message));
    (async () => $$t(await 1, 8))().then(v => results.push(v));
    async function* ag(a) {
        yield $$t(a, 8);
        yield $$t(await a, 16);
    }
    (async () => {
        try {
            for await (let v of ag(1))
                results.push("ag" + v);
        } catch (e) {
            results.push("ag " + e.message);
        }
    })();
    drainMicrotasks();
    shouldBe(results.sort().join(";"), [
        1,
        "Type check failed: expected number, got string",
        "Type check failed: expected number, got string",
        "ag Type check failed: expected string, got number",
        "ag1",
    ].join(";"));
})();

shouldBe(eval("$$t(1, 8)"), 1);
shouldBe(eval("$$t(1, 8);"), 1);
shouldBe(eval("2; if (true) $$t(1, 8);"), 1);
shouldBe((0, eval)("var soundTypesVar = $$t(1, 8); soundTypesVar"), 1);
shouldThrowTypeCheck(() => eval("$$t(1, 16)"), "Type check failed: expected string, got number");
shouldBe(new Function("a", "return $$t(a, 8)")(1), 1);
shouldBe(runString("var r = $$t(1, 8);").r, 1);
shouldBe((function (a) { return eval("$$t(a, 8)"); })(1), 1);

(function () {
    let sum = 0;
    for (let i = $$t(0, 8); $$t(i < 3, 4); $$t(i++, 8))
        sum += $$t(i, 8);
    shouldBe(sum, 3);
    for (let k in $$t({ a: 1 }, 512))
        shouldBe($$t(k, 16), "a");
    for (let v of $$t([1], 256))
        shouldBe($$t(v, 8), 1);
    switch ($$t(1, 8)) {
    case $$t(1, 8):
        sum = 10;
    }
    shouldBe(sum, 10);
    while ($$t(sum, 8) > 5)
        sum--;
    shouldBe(sum, 5);
    if ($$t(0, 8) || $$t("", 16) || !$$t(1, 8))
        throw new Error("bad condition");
})();
