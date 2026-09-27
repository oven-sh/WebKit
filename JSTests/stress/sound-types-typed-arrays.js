//@ requireOptions("--useSoundTypes=1")
// A mask may say which type of typed array the objects it lets by are: 512 (other objects) and, above the tags, one more than how far
// the type is from Int8Array.
function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error("bad value: " + actual + ", expected " + expected);
}
function shouldThrow(f, message) {
    let error;
    try { f(); } catch (e) { error = e; }
    if (!(error instanceof TypeError) || String(error.message) !== message)
        throw new Error("bad error: " + error + ", expected " + message);
}

const kinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float16Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array];

function int32(a) { return $$t(a, 6656); }
function float64(a) { return $$t(a, 10752); }
function float64OrNull(a) { return $$t(a, 10754); }
function uint8OrArray(a) { return $$t(a, 2816); }
function anyObject(a) { return $$t(a, 512); }

for (let i = 0; i < 200; i++) {
    for (const kind of kinds) {
        const a = new kind(2);
        shouldBe(anyObject(a), a);
        if (kind === Int32Array)
            shouldBe(int32(a), a);
        else
            shouldThrow(() => int32(a), "Type check failed: expected Int32Array, got object");
        if (kind === Float64Array) {
            shouldBe(float64(a), a);
            shouldBe(float64OrNull(a), a);
        } else {
            shouldThrow(() => float64(a), "Type check failed: expected Float64Array, got object");
            shouldThrow(() => float64OrNull(a), "Type check failed: expected Float64Array | null, got object");
        }
        if (kind === Uint8Array)
            shouldBe(uint8OrArray(a), a);
        else
            shouldThrow(() => uint8OrArray(a), "Type check failed: expected Uint8Array | array, got object");
    }
    shouldBe(float64OrNull(null), null);
    shouldThrow(() => float64(null), "Type check failed: expected Float64Array, got null");
    shouldThrow(() => float64({}), "Type check failed: expected Float64Array, got object");
    shouldThrow(() => float64([]), "Type check failed: expected Float64Array, got array");
    shouldThrow(() => float64(1), "Type check failed: expected Float64Array, got number");
    shouldThrow(() => float64(new DataView(new ArrayBuffer(8))), "Type check failed: expected Float64Array, got object");
    const array = [];
    shouldBe(uint8OrArray(array), array);
    class Sub extends Float64Array { }
    const sub = new Sub(1);
    shouldBe(float64(sub), sub);
}

// What the static compiler makes of knowing.
function sum(a, n) {
    $$t(a, 10752);
    let s = 0;
    for (let i = 0; i < n; i++)
        s += a[i];
    return s;
}
shouldBe(sum(new Float64Array([1.5, 2.5, 3]), 3), 7);
shouldBe(sum(new Float64Array([1.5, 2.5, 3]), 4), NaN);
shouldBe(sum(new Float64Array(0), 0), 0);

function fill(a, n, v) {
    $$t(a, 6656);
    for (let i = 0; i < n; i++)
        a[i] = v;
    let s = 0;
    for (let i = 0; i < a.length; i++)
        s += a[i];
    return s;
}
shouldBe(fill(new Int32Array(4), 4, 3), 12);
shouldBe(fill(new Int32Array(4), 6, 3), 12);
shouldBe(fill(new Int32Array(4), 4, 2 ** 32 + 1.5), 4);
shouldBe(fill(new Int32Array(4), 4, "5"), 20);
shouldBe(fill(new Int32Array(4), 4, NaN), 0);

function bytes(a) {
    $$t(a, 2560);
    let s = 0;
    for (let i = a.length - 1; i >= -1; i--)
        s += a[i] === undefined ? 1000 : a[a[i] & 3];
    return s;
}
shouldBe(bytes(new Uint8Array([1, 2, 3, 0])), 1000 + 1 + 0 + 3 + 2);

function unsigned(a) {
    $$t(a, 7680);
    let s = 0;
    for (let i = 0; i < a.length; i++)
        s += a[i];
    return s;
}
shouldBe(unsigned(new Uint32Array([4294967295, 4294967295, 1])), 8589934591);

function nans(a, b) {
    $$t(a, 10752);
    const out = [];
    for (let i = 0; i < a.length; i++)
        out.push(a[i]);
    return out;
}
const impure = new Float64Array(new BigUint64Array([0xfffe000000000001n, 0xffff000000000002n, 0x7ff8000000000000n]).buffer);
for (const value of nans(impure))
    shouldBe(value, NaN);

function detaches(a, n) {
    $$t(a, 10752);
    let s = 0;
    for (let i = 0; i < n; i++) {
        if (i === 2)
            a.buffer.transfer();
        s += a[i] === undefined ? 100 : a[i];
    }
    return s;
}
shouldBe(detaches(new Float64Array([1, 2, 3, 4]), 4), 203);

function resizable(a, buffer) {
    $$t(a, 2560);
    let s = 0;
    for (let i = 0; i < 6; i++) {
        if (i === 3)
            buffer.resize(8);
        s += a[i] === undefined ? 100 : a[i] + 1;
    }
    return s;
}
const growable = new ArrayBuffer(2, { maxByteLength: 16 });
shouldBe(resizable(new Uint8Array(growable), growable), 1 + 1 + 100 + 1 + 1 + 1);
