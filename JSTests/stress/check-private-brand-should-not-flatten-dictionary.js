//@ requireOptions("--useConcurrentJIT=0")

// DFG and FTL model CheckPrivateBrand as writing nothing, so they go on using a butterfly that was
// loaded before it. Flattening a dictionary whose out-of-line capacity shrinks replaces the butterfly,
// so the check's inline cache miss must not do that in their code.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

function makeUncacheableDictionary(C) {
    const o = new C;
    o[0] = o[1] = 2;
    for (let i = 0; i < 100; i++)
        o["k" + i] = i;
    for (let i = 0; i < 98; i++)
        delete o["k" + i];
    return o;
}

function test(name, makeClass) {
    // Two evaluations of one class body share f's code and give its check two brands, which keeps the
    // parser from turning the check into a CheckStructure.
    const A = makeClass();
    const B = makeClass();
    noInline(A.f);

    const a = new A;
    const b = new B;
    a[0] = a[1] = b[0] = b[1] = 2;
    for (let i = 0; i < testLoopCount; i++) {
        A.f(a, 7);
        B.f(b, 7);
    }

    const d = makeUncacheableDictionary(A);
    shouldBe(A.f(d, 7), 2, name);
    shouldBe(d[0], 2, name);
    shouldBe(d[1], 7, name);
    shouldBe(d.k98, 98, name);
    shouldBe(d.k99, 99, name);

    let threw = false;
    try {
        B.f(makeUncacheableDictionary(A), 7);
    } catch (e) {
        threw = e instanceof TypeError;
    }
    shouldBe(threw, true, name);
}

test("method reference", () => class {
    #m() { }
    static f(o, v) { const a = o[0]; o.#m; o[1] = v; return a; }
});

test("method call", () => class {
    #m() { }
    static f(o, v) { const a = o[0]; o.#m(); o[1] = v; return a; }
});

test("getter", () => class {
    get #g() { return 1; }
    static f(o, v) { const a = o[0]; o.#g; o[1] = v; return a; }
});

test("setter", () => class {
    set #s(x) { }
    static f(o, v) { const a = o[0]; o.#s = v; o[1] = v; return a; }
});

test("check in a loop", () => class {
    #m() { }
    static f(o, v) {
        const a = o[0];
        for (let i = 0; i < 2; i++) {
            o.#m;
            o[1] = v;
        }
        return a;
    }
});

// A miss in Baseline code does flatten. To an optimized caller that is a call, after which it loads the
// butterfly again.
(function () {
    class C {
        #m() { }
        static check(o) { o.#m; }
    }
    noDFG(C.check);
    noInline(C.check);

    function f(o, v) { const a = o[0]; C.check(o); o[1] = v; return a; }
    noInline(f);

    const c = new C;
    c[0] = c[1] = 2;
    for (let i = 0; i < testLoopCount; i++)
        f(c, 7);

    const d = makeUncacheableDictionary(C);
    shouldBe(f(d, 7), 2, "check in a Baseline callee");
    shouldBe(d[1], 7, "check in a Baseline callee");
    shouldBe(d.k99, 99, "check in a Baseline callee");
})();
