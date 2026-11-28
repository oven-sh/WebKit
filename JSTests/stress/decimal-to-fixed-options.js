//@ requireOptions("--useDecimal=1")
// toFixed({ ... }), where optimized code reads the options itself.

function shouldBe(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(`${what}: expected ${String(expected)}, got ${String(actual)}`);
}

const x = new Decimal("1.2550");
const outcome = f => { try { return f(); } catch (error) { return error.name; } };
let log = "";
function change(options, how) { how?.(options); }
noInline(change);

const cases = [
    ["x.toFixed()", "1"],
    ["x.toFixed(undefined)", "1"],
    ["x.toFixed({ digits: 2 })", "1.26"],
    ["x.toFixed({ digits: v })", "1.26"],
    ["x.toFixed({ digits: 2, roundingMode: 'floor' })", "1.25"],
    ["x.toFixed({ roundingMode: 'floor', other: 1, digits: v })", "1.25"],
    ["x.toFixed({ digits: 2, roundingMode: undefined })", "1.26"],
    ["x.toFixed({ a: 1, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1, h: 1, digits: 1 })", "1.3"],
    ["const o = { }; o.a = o.b = o.c = o.d = o.e = o.f = o.g = o.h = 1; o.digits = 1; return x.toFixed(o)", "1.3"],
    ["x.toFixed({ digits: 1, digits: 3 })", "1.255"],
    ["x.toFixed({ digits: 40 })", "1.255" + "0".repeat(37)],
    ["x.toFixed({ })", "TypeError"],
    ["x.toFixed({ digits: '2' })", "TypeError"],
    ["x.toFixed({ digits: -1 })", "RangeError"],
    ["x.toFixed({ digits: 1.5 })", "RangeError"],
    ["x.toFixed({ digits: 2, roundingMode: 'up' })", "RangeError"],
    ["x.toFixed({ digits: 2, roundingMode: 'flo' + s })", "1.25"],
    ["x.toFixed({ digits: -1 }); return 'unused'", "RangeError"],
    ["try { return x.toFixed({ digits: -1 }); } catch { return 'caught'; }", "caught"],
    ["x.toFixed({ __proto__: { digits: 1 } })", "1.3"],
    ["x.toFixed({ __proto__: null, digits: 1 })", "1.3"],
    ["x.toFixed({ get roundingMode() { log('m'); return 'ceil'; }, get digits() { log('d'); return 1; } })", "1.3", "dm"],
    ["x.toFixed({ digits: 1, get roundingMode() { log('m'); return 'floor'; } })", "1.2", "m"],
    ["x.toFixed(null)", "TypeError"],
    ["x.toFixed(2)", "TypeError"],
];
for (const [source, expected, expectedLog = ""] of cases) {
    const f = new Function("x", "v", "s", "log", /return/.test(source) && !/get /.test(source) ? source : "return " + source);
    noInline(f);
    for (let i = 0; i < testLoopCount / 2; ++i) {
        log = "";
        shouldBe(outcome(() => f(x, 2, "or", text => log += text)), expected, source);
        shouldBe(log, expectedLog, source);
    }
    shouldBe(outcome(() => f({ toFixed: Decimal.prototype.toFixed }, 2, "or", () => { })), /caught/.test(source) ? "caught" : "TypeError", source);
}

// The object changes between the literal and the call.
for (const [how, expected] of [
    [o => o.digits = 1, "1.3"],
    [o => o.roundingMode = "floor", "1.25"],
    [o => delete o.digits, "TypeError"],
    [o => Object.defineProperty(o, "digits", { get() { return 3; } }), "1.255"],
    [o => Object.setPrototypeOf(o, { roundingMode: "floor" }), "1.25"],
]) {
    const f = new Function("x", "change", "how", `const o = { digits: 2 }; change(o, how); return x.toFixed(o); // ${how}`);
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(f(x, change), "1.26", String(how));
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(outcome(() => f(x, change, how)), expected, String(how));
}

// Object.prototype gets the option, last, because it is only once that it has never had it.
{
    function f(x) { return x.toFixed({ digits: 2 }); }
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(f(x), "1.26", "before");
    Object.prototype.roundingMode = "floor";
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(f(x), "1.25", "inherited");
    delete Object.prototype.roundingMode;
    shouldBe(f(x), "1.26", "after");
}
