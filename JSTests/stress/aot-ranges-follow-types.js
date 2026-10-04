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
const integers = "integer-arithmetic";

function withLength(array, length) {
    Object.defineProperty(array, "length", { value: length });
    return array;
}

function uint8LengthPlusOne(a) { $$t(a, 2560); const n = $$t(a.length, 8); return n + 1; }
function int32LengthPlusOne(a) { $$t(a, 6656); const n = $$t(a.length, 8); return n + 1; }
function float64LengthPlusOne(a) { $$t(a, 10752); const n = $$t(a.length, 8); return n + 1; }
function lengthOfEitherPlusOne(a) { $$t(a, 2816); const n = $$t(a.length, 8); return n + 1; }
function inverseOfLength(a) { $$t(a, 2560); const n = a.length; if (typeof n === "number") return 1 / n; return "no number"; }
function lengthOrUndefined(a) { $$t(a, 2560); const n = $$t(a.length, 9); return n === undefined ? "undefined" : n * 2; }
function lengthTimesTwo(a) { $$t(a, 2560); return $$t(a.length, 8) * 2; }
function lengthIsInteger(a) { $$t(a, 2560); const n = $$t(a.length, 8); return n === Math.floor(n); }
function countsUpToLength(a) { $$t(a, 2560); const n = $$t(a.length, 8); let count = 0; for (let i = 0; i < n; i++) count++; return count; }
function countsWhileBelowLength(a) { $$t(a, 2560); let count = 0; for (let i = 0; i < a.length; i++) count += a[i] === undefined ? 100 : 1; return count; }
for (let f of [uint8LengthPlusOne, int32LengthPlusOne, float64LengthPlusOne, lengthOfEitherPlusOne, inverseOfLength, lengthOrUndefined, lengthTimesTwo, lengthIsInteger, countsUpToLength, countsWhileBelowLength])
    noInline(f);

check(uint8LengthPlusOne(new Uint8Array(4)), 5, "the length of a Uint8Array");
check(int32LengthPlusOne(new Int32Array(4)), 5, "the length of an Int32Array");
check(float64LengthPlusOne(new Float64Array(4)), 5, "the length of a Float64Array");
check(lengthOfEitherPlusOne(new Uint8Array(4)), 5, "the length of a Uint8Array or an array");
check(lengthOfEitherPlusOne([1, 2, 3]), 4, "the length of an array or a Uint8Array");
check(uint8LengthPlusOne(withLength(new Uint8Array(4), 1.5)), 2.5, "a fraction for a length");
check(int32LengthPlusOne(withLength(new Int32Array(4), 1.5)), 2.5, "a fraction for the length of an Int32Array");
check(float64LengthPlusOne(withLength(new Float64Array(4), -2.25)), -1.25, "a negative fraction for the length of a Float64Array");
check(lengthOfEitherPlusOne(withLength(new Uint8Array(4), 0.5)), 1.5, "a fraction for the length of a Uint8Array or an array");
check(uint8LengthPlusOne(withLength(new Uint8Array(4), NaN)), NaN, "NaN for a length");
check(uint8LengthPlusOne(withLength(new Uint8Array(4), Infinity)), Infinity, "Infinity for a length");
check(uint8LengthPlusOne(withLength(new Uint8Array(4), -Infinity)), -Infinity, "-Infinity for a length");
check(uint8LengthPlusOne(withLength(new Uint8Array(4), 1e300)), 1e300, "a huge length");
check(uint8LengthPlusOne(withLength(new Uint8Array(4), -7)), -6, "a negative length");
check(inverseOfLength(new Uint8Array(4)), 0.25, "the inverse of a length");
check(inverseOfLength(new Uint8Array(0)), Infinity, "the inverse of no length");
check(inverseOfLength(withLength(new Uint8Array(4), -0)), -Infinity, "negative zero for a length");
check(inverseOfLength(withLength(new Uint8Array(4), "4")), "no number", "a string for a length");
check(lengthOrUndefined(new Uint8Array(3)), 6, "a length or undefined");
check(lengthOrUndefined(withLength(new Uint8Array(3), undefined)), "undefined", "undefined for a length");
check(lengthOrUndefined(withLength(new Uint8Array(3), 0.75)), 1.5, "a fraction for a length or undefined");
check(lengthTimesTwo(withLength(new Uint8Array(4), 0.25)), 0.5, "a fraction, doubled");
check(lengthTimesTwo(withLength(new Uint8Array(4), -0)), -0, "negative zero, doubled");
check(lengthIsInteger(new Uint8Array(4)), true, "a length is an integer");
check(lengthIsInteger(withLength(new Uint8Array(4), 2.5)), false, "a fraction is no integer");
check(countsUpToLength(new Uint8Array(4)), 4, "counting up to a length");
check(countsUpToLength(withLength(new Uint8Array(4), 2.5)), 3, "counting up to a fraction");
check(countsUpToLength(withLength(new Uint8Array(4), NaN)), 0, "counting up to NaN");
check(countsWhileBelowLength(new Uint8Array(4)), 4, "counting while below a length");
check(countsWhileBelowLength(withLength(new Uint8Array(4), 2.5)), 3, "counting while below a fraction");
check(countsWhileBelowLength(withLength(new Uint8Array(4), 5.5)), 204, "counting while below a fraction beyond the end");

class HalfLonger extends Uint8Array {
    get length() { return super.length + 0.5; }
}
check(uint8LengthPlusOne(new HalfLonger(4)), 5.5, "a getter of a subclass");
check(countsUpToLength(new HalfLonger(2)), 3, "counting up to what a getter of a subclass says");
const reparented = new Uint8Array(4);
Object.setPrototypeOf(reparented, { length: 0.125 });
check(uint8LengthPlusOne(reparented), 1.125, "another prototype");
check(lengthTimesTwo(reparented), 0.25, "another prototype, doubled");

function uint8AtKey(a, k) { $$t(a, 2560); const v = $$t(a[k], 8); return v + 0.5; }
function int8AtKeyTimesBig(a, k) { $$t(a, 1536); const v = $$t(a[k], 8); return v * 16777215; }
function uint8AtKeyTimesBig(a, k) { $$t(a, 2560); const v = $$t(a[k], 8); return v * 8388607; }
function uint16AtKeyNegated(a, k) { $$t(a, 5632); const v = a[k]; if (typeof v === "number") return -v; return "no number"; }
function uint32AtKeyPlusOne(a, k) { $$t(a, 7680); const v = $$t(a[k], 8); return v + 1; }
function int32AtKeyOrUndefined(a, k) { $$t(a, 6656); const v = $$t(a[k], 9); return v === undefined ? "undefined" : v / 2; }
for (let f of [uint8AtKey, int8AtKeyTimesBig, uint8AtKeyTimesBig, uint16AtKeyNegated, uint32AtKeyPlusOne, int32AtKeyOrUndefined])
    noInline(f);

const bytes = new Uint8Array(1000);
bytes[1] = 200;
bytes.ratio = 1.5;
bytes.nothing = -0;
bytes.nan = NaN;
const symbol = Symbol();
bytes[symbol] = 2.25;
check(uint8AtKey(bytes, 1), 200.5, "an element");
check(uint8AtKey(bytes, "1"), 200.5, "an element by a string");
check(uint8AtKey(bytes, "ratio"), 2, "a property that is a fraction");
check(uint8AtKey(bytes, "nan"), NaN, "a property that is NaN");
check(uint8AtKey(bytes, symbol), 2.75, "a property named by a symbol");
check(uint8AtKey(bytes, "length"), 1000.5, "length by a key");
check(uint8AtKey(bytes, { toString() { return "ratio"; } }), 2, "a key that is an object");
check(uint8AtKeyTimesBig(bytes, 1), 1677721400, "an element times a big number");
check(uint8AtKeyTimesBig(bytes, "length"), 8388607000, "length by a key, times a big number");
check(uint8AtKeyTimesBig(bytes, "byteLength"), 8388607000, "byteLength by a key, times a big number");
const signedBytes = new Int8Array(300);
signedBytes[2] = -128;
check(int8AtKeyTimesBig(signedBytes, 2), -2147483520, "a negative element times a big number");
check(int8AtKeyTimesBig(signedBytes, "length"), 5033164500, "the length of an Int8Array by a key, times a big number");
const shorts = new Uint16Array(4);
shorts[3] = 65535;
shorts.nothing = 0;
shorts.half = 0.5;
check(uint16AtKeyNegated(shorts, 3), -65535, "an element, negated");
check(uint16AtKeyNegated(shorts, 0), -0, "zero, negated");
check(uint16AtKeyNegated(shorts, "nothing"), -0, "a property that is zero, negated");
check(uint16AtKeyNegated(shorts, "half"), -0.5, "a property that is a fraction, negated");
check(uint16AtKeyNegated(shorts, 4), "no number", "beyond the end");
const words = new Uint32Array(2);
words[0] = 4294967295;
words.big = 2 ** 60;
words.small = -1.5;
check(uint32AtKeyPlusOne(words, 0), 4294967296, "the largest element");
check(uint32AtKeyPlusOne(words, "big"), 2 ** 60, "a property beyond 2^53");
check(uint32AtKeyPlusOne(words, "small"), -0.5, "a property below zero");
const ints = new Int32Array(2);
ints[1] = -2147483648;
ints.third = 1 / 3;
check(int32AtKeyOrUndefined(ints, 1), -1073741824, "the smallest element");
check(int32AtKeyOrUndefined(ints, 2), "undefined", "beyond the end, or undefined");
check(int32AtKeyOrUndefined(ints, "third"), 1 / 6, "a property that is a fraction, or undefined");
check(int32AtKeyOrUndefined(ints, "fourth"), "undefined", "no such property");

function uint8AtIndexPlusOne(a, i) { $$t(a, 2560); $$t(i, 8); const v = $$t(a[i], 8); return v + 1; }
function uint32AtIndexPlusOne(a, i) { $$t(a, 7680); $$t(i, 8); const v = $$t(a[i], 8); return v + 1; }
function arrayLengthPlusOne(a) { $$t(a, 256); return a.length + 1; }
function stringLengthPlusOne(s) { $$t(s, 16); return s.length + 1; }
function stringOrArrayLengthPlusOne(a) { $$t(a, 272); return a.length + 1; }
function sumsBytes(a) { $$t(a, 2560); let s = 0; for (let i = 0; i < a.length; i++) s = (s + a[i]) | 0; return s; }
for (let f of [uint8AtIndexPlusOne, uint32AtIndexPlusOne, arrayLengthPlusOne, stringLengthPlusOne, stringOrArrayLengthPlusOne, sumsBytes])
    noInline(f);

check(uint8AtIndexPlusOne(bytes, 1), 201, "an element at an index");
check(uint32AtIndexPlusOne(words, 0), 4294967296, "the largest element at an index");
check(arrayLengthPlusOne([1, 2, 3]), 4, "the length of an array");
const longest = [];
longest.length = 4294967295;
check(arrayLengthPlusOne(longest), 4294967296, "the length of the longest array");
check(stringLengthPlusOne("four"), 5, "the length of a string");
check(stringOrArrayLengthPlusOne("four"), 5, "the length of a string or an array");
check(stringOrArrayLengthPlusOne(longest), 4294967296, "the length of an array or a string");
check(sumsBytes(bytes), 200, "a sum of bytes");
check(sumsBytes(withLength(new Uint8Array([1, 2, 3, 4]), 2.5)), 6, "a sum of bytes up to a fraction");
check(sumsBytes(new HalfLonger([1, 2, 3])), 0, "a sum of bytes that goes beyond the end");

applies(uint8AtIndexPlusOne, integers);
applies(uint32AtIndexPlusOne, integers);
applies(arrayLengthPlusOne, integers);
applies(stringLengthPlusOne, integers);
applies(stringOrArrayLengthPlusOne, integers);
function readsProperty(o) { return o.property; }
noInline(readsProperty);
readsProperty({ property: 1 });
const usesDataStubs = (remarksOf(readsProperty) || []).includes("calls:GetById");
if (usesDataStubs) {
    applies(sumsBytes, "split-loop");
    applies(sumsBytes, integers);
}
for (let f of [uint8LengthPlusOne, int32LengthPlusOne, float64LengthPlusOne, lengthOfEitherPlusOne, lengthOrUndefined, lengthTimesTwo, uint8AtKey, int8AtKeyTimesBig, uint8AtKeyTimesBig, uint32AtKeyPlusOne])
    doesNotApply(f, integers);

const typedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype);
const lengthOfTypedArrays = Object.getOwnPropertyDescriptor(typedArrayPrototype, "length");
Object.defineProperty(typedArrayPrototype, "length", { get() { return lengthOfTypedArrays.get.call(this) - 0.5; }, configurable: true });
try {
    check(uint8LengthPlusOne(new Uint8Array(4)), 4.5, "another getter for all typed arrays");
    check(countsUpToLength(new Uint8Array(4)), 4, "counting up to what another getter says");
    check(countsWhileBelowLength(new Uint8Array(4)), 4, "counting while below what another getter says");
    check(sumsBytes(new Uint8Array([1, 2, 3, 4])), 10, "a sum of bytes up to what another getter says");
    check(uint8AtKey(new Uint8Array(4), "length"), 4, "another getter, by a key");
} finally {
    Object.defineProperty(typedArrayPrototype, "length", lengthOfTypedArrays);
}
check(uint8LengthPlusOne(new Uint8Array(4)), 5, "the first getter again");
