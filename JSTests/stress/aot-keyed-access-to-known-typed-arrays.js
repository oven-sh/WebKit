//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1", "--validateAOTInferredTypes=1")

function shouldBe(actual, expected, what)
{
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function shouldThrow(run, constructor, what)
{
    let error = null;
    try {
        run();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof constructor))
        throw new Error(what + ": " + String(error) + " instead of a " + constructor.name);
}

function readsInt8(a, i) { return $$t(a, 1536)[i]; }
function readsUint8(a, i) { return $$t(a, 2560)[i]; }
function readsInt16(a, i) { return $$t(a, 4608)[i]; }
function readsUint16(a, i) { return $$t(a, 5632)[i]; }
function readsInt32(a, i) { return $$t(a, 6656)[i]; }
function readsUint32(a, i) { return $$t(a, 7680)[i]; }
function readsFloat32(a, i) { return $$t(a, 9728)[i]; }
function readsFloat64(a, i) { return $$t(a, 10752)[i]; }

function writesInt8(a, i, v) { return $$t(a, 1536)[i] = v; }
function writesUint8(a, i, v) { return $$t(a, 2560)[i] = v; }
function writesInt16(a, i, v) { return $$t(a, 4608)[i] = v; }
function writesUint16(a, i, v) { return $$t(a, 5632)[i] = v; }
function writesInt32(a, i, v) { return $$t(a, 6656)[i] = v; }
function writesUint32(a, i, v) { return $$t(a, 7680)[i] = v; }
function writesFloat32(a, i, v) { return $$t(a, 9728)[i] = v; }
function writesFloat64(a, i, v) { return $$t(a, 10752)[i] = v; }

function readsWithNumber(a, i) { return $$t(a, 6656)[$$t(i, 8)]; }
function readsWithInteger(a, i) { return $$t(a, 6656)[i | 0]; }
function readsWithWideInteger(a, i) { return $$t(a, 6656)[(i | 0) + 2147483647]; }
function writesNumber(a, i, v) { "use strict"; $$t(a, 6656)[i | 0] = $$t(v, 8); }
function writesInteger(a, i, v) { "use strict"; $$t(a, 10752)[i | 0] = v | 0; }
function updates(a, i) { const array = $$t(a, 6656); array[i] += 2; array[i + 1]++; return array[i] + array[i + 1]; }
function readsNullable(a, i) { return $$t(a, 6658)[i]; }
function readsOptional(a, i) { return $$t(a, 6657)[i]; }
function writesNullable(a, i, v) { "use strict"; $$t(a, 6659)[i] = v; }
const [readsCaptured, writesCaptured] = (() => {
    const scratch = $$t(new Int32Array([1, 2, 3]), 6656);
    return [function readsCaptured(i) { return scratch[i]; }, function writesCaptured(i, v) { scratch[i] = v; }];
})();
const [readsLate, writesLate, fills] = (() => {
    let late = $$t(undefined, 6657);
    return [function readsLate(i) { return late[i]; }, function writesLate(i, v) { late[i] = v; }, function fills() { late = $$t(new Int32Array([7, 8]), 6657); }];
})();
function addsCells(a, w, x, y) { const cells = $$t(a, 6656); return cells[y * w + x] + cells[y * w + x + 1]; }

function readsUnknown(a, i) { return a[i]; }
function writesUnknown(a, i, v) { a[i] = v; }
function readsNumberOrArray(a, i) { return $$t(a, 6664)[i]; }
function readsEither(a, i) { return $$t(a, 2816)[i]; }
function readsClamped(a, i) { return $$t(a, 3584)[i]; }
function writesClamped(a, i, v) { $$t(a, 3584)[i] = v; }
function readsFloat16(a, i) { return $$t(a, 8704)[i]; }
function readsBigInt64(a, i) { return $$t(a, 11776)[i]; }
function writesBigInt64(a, i, v) { $$t(a, 11776)[i] = v; }
function readsWithString(a, k) { return $$t(a, 6656)[$$t(k, 16)]; }
function writesString(a, i, v) { $$t(a, 6656)[i] = $$t(v, 16); }
function readsLength(a) { return $$t(a, 6656)["length"]; }

const kinds = [
    [Int8Array, readsInt8, writesInt8, v => v << 24 >> 24],
    [Uint8Array, readsUint8, writesUint8, v => v & 255],
    [Int16Array, readsInt16, writesInt16, v => v << 16 >> 16],
    [Uint16Array, readsUint16, writesUint16, v => v & 65535],
    [Int32Array, readsInt32, writesInt32, v => v | 0],
    [Uint32Array, readsUint32, writesUint32, v => v >>> 0],
    [Float32Array, readsFloat32, writesFloat32, v => Math.fround(v)],
    [Float64Array, readsFloat64, writesFloat64, v => +v],
];
const numbers = [0, -0, 1, -1, 127, 128, 255, 256, 300, -129, 32767, 32768, 65535, 65536, 2147483647, 2147483648, -2147483648, -2147483649, 4294967295, 4294967296, 4294967301, 9007199254740992, 1.9, -1.9, 0.1, 1e300, -1e300, NaN, Infinity, -Infinity];
const noIndices = [4, 5, -1, 1.5, -1.5, NaN, Infinity, -Infinity, 2147483647, 2147483648, -2147483648, 4294967296, 4294967297, 9007199254740991, 1e300];

for (let round = 0; round < 3; ++round) {
    for (const [constructor, reads, writes, converts] of kinds) {
        const name = constructor.name;
        const array = new constructor(4);
        for (const number of numbers) {
            shouldBe(writes(array, 1, number), number, name + ": the value of an assignment is what was assigned");
            shouldBe(reads(array, 1), converts(number), name + ": " + number + " read back");
            shouldBe(array[1], converts(number), name + ": " + number + " seen by other code");
            shouldBe(reads(array, 0), 0, name + ": the element before");
            shouldBe(reads(array, 2), 0, name + ": the element after");
        }
        writes(array, 0, 11);
        writes(array, 3, 44);
        shouldBe(reads(array, 0), 11, name + ": the first element");
        shouldBe(reads(array, -0), 11, name + ": negative zero is the first index");
        shouldBe(reads(array, 3), 44, name + ": the last element");
        shouldBe(reads(array, 3.0), 44, name + ": the last element");
        shouldBe(reads(array, "3"), 44, name + ": an index as a string");
        shouldBe(reads(array, "-0"), undefined, name + ": the string -0 names nothing");
        shouldBe(reads(array, "length"), 4, name + ": a name");
        for (const index of noIndices) {
            shouldBe(reads(array, index), undefined, name + ": a read at " + index);
            shouldBe(writes(array, index, 9), 9, name + ": a store at " + index);
            shouldBe(Object.keys(array).join(), "0,1,2,3", name + ": a store at " + index + " makes no property");
        }
        shouldBe(reads(array, 0) + reads(array, 3), 55, name + ": stores out of bounds change nothing");

        shouldBe(writes(array, 2, "7"), "7", name + ": a string");
        shouldBe(reads(array, 2), 7, name + ": a string is converted");
        writes(array, 2, true);
        shouldBe(reads(array, 2), 1, name + ": true");
        writes(array, 2, null);
        shouldBe(reads(array, 2), 0, name + ": null");
        writes(array, 2, undefined);
        shouldBe(reads(array, 2), converts(NaN), name + ": undefined");
        shouldThrow(() => writes(array, 2, Symbol()), TypeError, name + ": a symbol");
        shouldThrow(() => writes(array, 2, 1n), TypeError, name + ": a BigInt");
        shouldThrow(() => writes(array, 100, 1n), TypeError, name + ": a BigInt out of bounds");

        const log = [];
        writes(array, 100, { valueOf() { log.push("converted"); return 1; } });
        shouldBe(log.join(), "converted", name + ": the value is converted before the index is looked at");

        const victim = new constructor(4);
        writes(victim, 1, 5);
        writes(victim, 1, { valueOf() { victim.buffer.transfer(); return 6; } });
        shouldBe(victim.length, 0, name + ": detached while the value was converted");
        shouldBe(reads(victim, 1), undefined, name + ": a read from a detached array");
        shouldBe(reads(victim, 0), undefined, name + ": a read from a detached array");
        shouldBe(writes(victim, 0, 3), 3, name + ": a store to a detached array");
        shouldBe(reads(victim, 0), undefined, name + ": a detached array stays empty");

        const size = constructor.BYTES_PER_ELEMENT;
        const resizable = new ArrayBuffer(4 * size, { maxByteLength: 16 * size });
        const tracking = new constructor(resizable);
        const fixed = new constructor(resizable, 0, 4);
        writes(tracking, 3, 33);
        shouldBe(reads(fixed, 3), 33, name + ": two arrays on one buffer");
        shouldBe(reads(tracking, 4), undefined, name + ": beyond a buffer that may grow");
        resizable.resize(8 * size);
        shouldBe(reads(tracking, 4), 0, name + ": within a buffer that has grown");
        writes(tracking, 7, 77);
        shouldBe(reads(tracking, 7), 77, name + ": a store within a buffer that has grown");
        shouldBe(reads(fixed, 4), undefined, name + ": an array of fixed length on a buffer that has grown");
        resizable.resize(2 * size);
        shouldBe(reads(tracking, 3), undefined, name + ": beyond a buffer that has shrunk");
        shouldBe(reads(fixed, 0), undefined, name + ": an array that no longer fits its buffer");
        shouldBe(writes(fixed, 0, 1), 1, name + ": a store to an array that no longer fits its buffer");
        shouldBe(reads(tracking, 0), 0, name + ": that store is ignored");
        resizable.resize(8 * size);
        shouldBe(reads(tracking, 3), 0, name + ": what was cut off is zero");

        const growable = new SharedArrayBuffer(2 * size, { maxByteLength: 8 * size });
        const sharedArray = new constructor(growable);
        shouldBe(reads(sharedArray, 2), undefined, name + ": beyond a shared buffer that may grow");
        growable.grow(4 * size);
        writes(sharedArray, 2, 22);
        shouldBe(reads(sharedArray, 2), 22, name + ": within a shared buffer that has grown");

        const small = new constructor(4);
        writes(small, 1, 12);
        shouldBe(small.buffer.byteLength, 4 * size, name + ": the buffer is asked for");
        shouldBe(reads(small, 1), 12, name + ": a read after the storage has moved");
        writes(small, 1, 13);
        shouldBe(new constructor(small.buffer)[1], 13, name + ": a store after the storage has moved");
        const part = small.subarray(1, 3);
        shouldBe(reads(part, 0), 13, name + ": part of an array");
        shouldBe(reads(part, 2), undefined, name + ": beyond part of an array");
        writes(part, 1, 14);
        shouldBe(reads(small, 2), 14, name + ": a store through part of an array");

        class Derived extends constructor { }
        const derived = new Derived(2);
        derived.extra = 1;
        writes(derived, 1, 21);
        shouldBe(reads(derived, 1), 21, name + ": an instance of a derived class");
        shouldBe(reads(derived, 2), undefined, name + ": beyond an instance of a derived class");
        shouldBe(reads(derived, "extra"), 1, name + ": a property of its own");

        const empty = new constructor(0);
        shouldBe(reads(empty, 0), undefined, name + ": an empty array");
        writes(empty, 0, 1);
        shouldBe(reads(empty, 0), undefined, name + ": an empty array");

        shouldThrow(() => reads(null, 0), TypeError, name + ": null");
        shouldThrow(() => reads([1], 0), TypeError, name + ": an array");
        shouldThrow(() => writes({ }, 0, 1), TypeError, name + ": an object");
    }
}

for (const holder of [Object.prototype, Int32Array.prototype, Object.getPrototypeOf(Int32Array.prototype)]) {
    try {
        Object.defineProperty(holder, "9", { value: "inherited", configurable: true });
    } catch { }
}
shouldBe(readsInt32(new Int32Array(4), 9), undefined, "an index is never looked for in the prototypes");
shouldBe(readsWithInteger(new Int32Array(4), 9), undefined, "an index is never looked for in the prototypes");

const cells = new Int32Array([10, 20, 30, 40, 50, 60]);
for (let i = 0; i < 100; ++i) {
    shouldBe(readsWithNumber(cells, 2), 30, "a key that is a number");
    shouldBe(readsWithNumber(cells, 2.5), undefined, "a key that is a fraction");
    shouldBe(readsWithNumber(cells, 6), undefined, "a key that is the length");
    shouldBe(readsWithInteger(cells, 5), 60, "a key that is an integer");
    shouldBe(readsWithInteger(cells, -1), undefined, "a key that is a negative integer");
    shouldBe(readsWithInteger(cells, 4294967297), 20, "a key that is made an integer");
    shouldBe(readsWithWideInteger(cells, -2147483647), 10, "a key that is a wide integer");
    shouldBe(readsWithWideInteger(cells, 2), undefined, "a key beyond thirty-two bits is not cut off");
    shouldBe(readsWithWideInteger(cells, 2147483647), undefined, "a key beyond thirty-two bits is not cut off");
    shouldBe(addsCells(cells, 3, 1, 1), 110, "keys that are computed");
    shouldBe(addsCells(cells, 3, 2, 1), NaN, "the second of the keys is out of bounds");
    shouldBe(addsCells(cells, 0.5, 1, 1), NaN, "keys that are fractions");
}
const counters = new Int32Array(3);
for (let i = 0; i < 100; ++i)
    shouldBe(updates(counters, 0), 3 * (i + 1), "elements that are updated");
shouldBe(updates(counters, 2), NaN, "an update out of bounds");
shouldBe(counters.join(), "200,100,2", "elements that were updated");
writesNumber(counters, 0, 1.9);
writesNumber(counters, 7, 1.9);
writesNumber(counters, -1, 1.9);
shouldBe(counters.join(), "1,100,2", "a number stored in strict code");
const doubles = new Float64Array(2);
writesInteger(doubles, 1, 7.5);
writesInteger(doubles, 2, 7.5);
shouldBe(doubles.join(), "0,7", "an integer stored as a double");

shouldBe(readsUnknown(cells, 1), 20, "a base that is not known");
writesUnknown(cells, 1, 21);
for (let i = 0; i < 100; ++i) {
    shouldBe(readsNullable(cells, 1), 21, "a base that may be null");
    shouldBe(readsNullable(cells, 6), undefined, "a base that may be null, out of bounds");
    shouldThrow(() => readsNullable(null, 1), TypeError, "a base that is null");
    shouldThrow(() => readsNullable(undefined, 1), TypeError, "undefined where only null is allowed");
    shouldBe(readsOptional(cells, 0), 10, "a base that may be undefined");
    shouldThrow(() => readsOptional(undefined, 0), TypeError, "a base that is undefined");
    writesNullable(cells, 1, 21.5);
    writesNullable(cells, 6, 1);
    shouldBe(cells[1], 21, "a store to a base that may be null");
    shouldThrow(() => writesNullable(null, 1, 1), TypeError, "a store to null");
    shouldThrow(() => writesNullable(undefined, 1, 1), TypeError, "a store to undefined");
    shouldThrow(() => writesNullable(null, 100, "1"), TypeError, "a store to null, out of bounds");
    shouldBe(readsCaptured(2), 3, "a variable of an outer function");
    shouldBe(readsCaptured(3), undefined, "a variable of an outer function, out of bounds");
    writesCaptured(0, i);
    shouldBe(readsCaptured(0), i, "a store through a variable of an outer function");
}
shouldThrow(() => readsLate(0), TypeError, "a variable that is still undefined");
shouldThrow(() => writesLate(0, 1), TypeError, "a store through a variable that is still undefined");
fills();
shouldBe(readsLate(1), 8, "a variable that was filled later");
writesLate(1, 9);
shouldBe(readsLate(1), 9, "a store through a variable that was filled later");
shouldBe(readsLate(2), undefined, "a variable that was filled later, out of bounds");
shouldBe(readsNumberOrArray(cells, 0), 10, "a base that may be a number");
shouldBe(readsNumberOrArray(5, 0), undefined, "a base that is a number");
shouldBe(readsEither(new Uint8Array([1, 2]), 1), 2, "a base of one of two kinds");
shouldBe(readsEither([1, 2], 1), 2, "a base of one of two kinds");
const clamped = new Uint8ClampedArray(2);
writesClamped(clamped, 0, 300);
writesClamped(clamped, 1, 1.5);
shouldBe(readsClamped(clamped, 0), 255, "a clamped element");
shouldBe(readsClamped(clamped, 1), 2, "a clamped element is rounded to even");
shouldBe(readsFloat16(new Float16Array([1.5]), 0), 1.5, "half precision");
const big = new BigInt64Array(1);
writesBigInt64(big, 0, 5n);
shouldBe(readsBigInt64(big, 0), 5n, "a BigInt element");
shouldBe(readsWithString(cells, "2"), 30, "a key that is a string");
shouldBe(readsWithString(cells, "byteLength"), 24, "a key that is a name");
writesString(cells, 2, "31");
shouldBe(cells[2], 31, "a value that is a string");
shouldBe(readsLength(cells), 6, "a constant name");

const remarksOf = f => typeof aotRemarks === "function" && typeof isAOTCompiled === "function" && isAOTCompiled(f) ? aotRemarks(f.name) || [] : null;
function applies(f, remark)
{
    const remarks = remarksOf(f);
    if (remarks && !remarks.includes(remark))
        throw new Error(remark + " does not apply to " + f.name + ": " + remarks.join(" "));
}
function doesNotApply(f)
{
    const remarks = remarksOf(f);
    for (const remark of ["get-by-val-on-typed-array", "put-by-val-on-typed-array"]) {
        if (remarks && remarks.includes(remark))
            throw new Error(remark + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
for (const [, reads, writes] of kinds) {
    applies(reads, "get-by-val-on-typed-array");
    applies(writes, "put-by-val-on-typed-array");
}
for (const f of [readsWithNumber, readsWithInteger, readsWithWideInteger, updates, addsCells, readsNullable, readsOptional, readsCaptured, readsLate])
    applies(f, "get-by-val-on-typed-array");
for (const f of [writesNumber, writesInteger, updates, writesNullable, writesCaptured, writesLate])
    applies(f, "put-by-val-on-typed-array");
for (const f of [readsUnknown, writesUnknown, readsNumberOrArray, readsEither, readsClamped, writesClamped, readsFloat16, readsBigInt64, writesBigInt64, readsWithString, writesString, readsLength])
    doesNotApply(f);
for (const f of [readsWithInteger, readsWithWideInteger]) {
    const remarks = remarksOf(f);
    if (remarks && remarks.includes("calls:GetByVal"))
        throw new Error(f.name + " has a call of the stub: " + remarks.join(" "));
}
