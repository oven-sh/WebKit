//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function applies(f, pattern) {
    let remarks = remarksOf(f);
    if (remarks && !remarks.includes(pattern))
        throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
}
function doesNotApply(f, pattern) {
    let remarks = remarksOf(f);
    if (remarks && remarks.includes(pattern))
        throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
}
const behindBranch = "double-to-int32-behind-branch";

function orZero(v) { const x = $$t(v, 12); return x | 0; }
function andAllBits(v) { const x = $$t(v, 12); return x & -1; }
function xorZero(v) { const x = $$t(v, 12); return x ^ 0; }
function shiftedLeft(v) { const x = $$t(v, 12); return x << 1; }
function shiftedRight(v) { const x = $$t(v, 12); return x >> 1; }
function shiftedRightUnsigned(v) { const x = $$t(v, 12); return x >>> 0; }
function asShiftCount(v) { const x = $$t(v, 12); return 1 << x; }
function inverted(v) { const x = $$t(v, 12); return ~x; }
function orOfBoth(v, w) { const x = $$t(v, 12); const y = $$t(w, 12); return x | y; }

function numberOrZero(v) { const x = $$t(v, 8); return x | 0; }
function booleanOrZero(v) { const x = $$t(v, 4); return x | 0; }
function booleanOrInt32OrZero(v, takesNumber) { const x = takesNumber ? v | 0 : true; return x | 0; }

const converter = new Int32Array(1);
function toInt32(value) {
    converter[0] = Number(value);
    return converter[0];
}

const values = [
    0, 1, 5, -7, 31, 32, 33, 2147483647, -2147483648,
    true, false,
    0.5, 5.5, -5.5, 1.9999, -0.9999, -0, 2147483647.5, -2147483648.5,
    2147483648, -2147483649, 4294967295, 4294967296, 4294967301, -4294967301,
    2 ** 40 + 3, -(2 ** 40 + 3), 2 ** 53, -(2 ** 53), 2 ** 63, -(2 ** 63), 2 ** 64 + 2 ** 12, 1e21, 1e300, -1e300,
    NaN, Infinity, -Infinity, Number.MIN_VALUE, Number.MAX_VALUE,
];

for (let round = 0; round < 3; round++) {
    for (const value of values) {
        const integer = toInt32(value);
        const what = String(value) + (Object.is(value, -0) ? " (negative)" : "");
        check(orZero(value), integer, what + " | 0");
        check(andAllBits(value), integer, what + " & -1");
        check(xorZero(value), integer, what + " ^ 0");
        check(shiftedLeft(value), integer << 1, what + " << 1");
        check(shiftedRight(value), integer >> 1, what + " >> 1");
        check(shiftedRightUnsigned(value), integer >>> 0, what + " >>> 0");
        check(asShiftCount(value), 1 << integer, "1 << " + what);
        check(inverted(value), ~integer, "~" + what);
        for (const other of values)
            check(orOfBoth(value, other), integer | toInt32(other), what + " | " + String(other));
        if (typeof value === "number") {
            check(numberOrZero(value), integer, what + " | 0, as a number");
            check(booleanOrInt32OrZero(value, true), integer, what + " | 0, twice");
        } else
            check(booleanOrZero(value), integer, what + " | 0, as a boolean");
    }
    check(booleanOrInt32OrZero(5, false), 1, "true | 0");
}

for (const f of [orZero, andAllBits, xorZero, shiftedLeft, shiftedRight, shiftedRightUnsigned, asShiftCount, orOfBoth])
    applies(f, behindBranch);
for (const f of [numberOrZero, booleanOrZero, booleanOrInt32OrZero])
    doesNotApply(f, behindBranch);
