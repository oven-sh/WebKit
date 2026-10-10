//@ runDefault("--compileMainScriptAheadOfTime=1")

(function () {
    "use strict";

    function shouldBe(actual, expected, what) {
        if (actual !== expected)
            throw new Error(what + ": expected " + expected + " but got " + actual);
    }

    function toInt32(x) {
        if (x !== x || x === Infinity || x === -Infinity)
            return 0;
        let integer = x < 0 ? -Math.floor(-x) : Math.floor(x);
        let modulo = integer % 4294967296;
        if (modulo < 0)
            modulo += 4294967296;
        return modulo >= 2147483648 ? modulo - 4294967296 : modulo;
    }

    function or(x) { return (x * 1.0) | 0; }
    function not(x) { return ~~(x * 1.0); }
    function shift(x) { return (x * 1.0) >> 0; }
    function unsignedShift(x) { return (x * 1.0) >>> 0; }
    function and(x) { return (x * 1.0) & -1; }
    function xor(x) { return (x * 1.0) ^ 0; }
    function amount(x) { return 1 << (x * 1.0); }
    function imul(x) { return Math.imul(x * 1.0, 1); }
    function unknown(x) { return x | 0; }
    for (const f of [or, not, shift, unsignedShift, and, xor, amount, imul, unknown])
        noInline(f);

    const two31 = 2147483648;
    const two32 = 4294967296;
    const two53 = 9007199254740992;
    const two63 = 9223372036854775808;
    const two64 = 18446744073709551616;
    const values = [
        0, -0, 0.5, -0.5, 0.999999, -0.999999, 1, -1, 1.5, -1.5, 3.7e10, -3.7e10,
        two31 - 1, two31 - 0.5, two31, two31 + 0.5, two31 + 1, -two31, -two31 - 0.5, -two31 - 1,
        two32 - 1, two32 - 0.5, two32, two32 + 1, two32 + 5.5, -two32, -two32 - 1, -two32 + 1,
        two53 - 1, two53, two53 + 2, -two53, -two53 - 2,
        two63 - 1024, two63, two63 + 2048, -two63, -two63 - 2048, -two63 + 1024,
        two64 - 2048, two64, two64 + 4096, -two64,
        two63 + two32 * 3, two64 + two32 * 4096 * 5, 1e19, -1e19, 1e20, 1e21, 1.2345e25, 1e300, -1e300,
        Number.MAX_VALUE, -Number.MAX_VALUE, Number.MIN_VALUE, -Number.MIN_VALUE, Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER,
        NaN, Infinity, -Infinity,
    ];

    for (let round = 0; round < 3; round++) {
        for (const value of values) {
            const expected = toInt32(value);
            shouldBe(or(value), expected, "or " + value);
            shouldBe(not(value), expected, "not " + value);
            shouldBe(shift(value), expected, "shift " + value);
            shouldBe(unsignedShift(value), expected < 0 ? expected + two32 : expected, "unsignedShift " + value);
            shouldBe(and(value), expected, "and " + value);
            shouldBe(xor(value), expected, "xor " + value);
            shouldBe(amount(value), 1 << (expected & 31), "amount " + value);
            shouldBe(imul(value), expected, "imul " + value);
            shouldBe(unknown(value), expected, "unknown " + value);
        }
    }

    shouldBe(Object.is(or(-0), 0), true, "the result is not negative zero");
    for (const f of [or, not, shift, and, xor])
        shouldBe(String(aotRemarks(f.name)).includes("compiled"), true, f.name + " is compiled");
})();
