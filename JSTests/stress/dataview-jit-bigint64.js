// Correctness test for DataView BigInt64/BigUint64 DFG/FTL intrinsics.
function assert(cond, msg) {
    if (!cond)
        throw new Error("FAIL: " + msg);
}

// Reference implementations via byte-level access.
function refGetBigUint64(bytes, offset, littleEndian) {
    let v = 0n;
    for (let i = 0; i < 8; i++) {
        const b = BigInt(bytes[offset + (littleEndian ? 7 - i : i)]);
        v = (v << 8n) | b;
    }
    return v;
}
function refGetBigInt64(bytes, offset, littleEndian) {
    const u = refGetBigUint64(bytes, offset, littleEndian);
    return u >= (1n << 63n) ? u - (1n << 64n) : u;
}
function refSetBigUint64(bytes, offset, value, littleEndian) {
    let v = ((value % (1n << 64n)) + (1n << 64n)) % (1n << 64n);
    for (let i = 0; i < 8; i++) {
        const shift = BigInt(8 * (littleEndian ? i : 7 - i));
        bytes[offset + i] = Number((v >> shift) & 0xffn);
    }
}

// --- get tests: constant endianness (true / false / omitted) and variable ---
function testGetLE(view, offset) { return [view.getBigInt64(offset, true), view.getBigUint64(offset, true)]; }
function testGetBE(view, offset) { return [view.getBigInt64(offset, false), view.getBigUint64(offset, false)]; }
function testGetDefault(view, offset) { return [view.getBigInt64(offset), view.getBigUint64(offset)]; }
function testGetVar(view, offset, le) { return [view.getBigInt64(offset, le), view.getBigUint64(offset, le)]; }
noInline(testGetLE);
noInline(testGetBE);
noInline(testGetDefault);
noInline(testGetVar);

function testSetLE(view, offset, v1, v2) { view.setBigInt64(offset, v1, true); view.setBigUint64(offset + 8, v2, true); }
function testSetBE(view, offset, v1, v2) { view.setBigInt64(offset, v1, false); view.setBigUint64(offset + 8, v2, false); }
function testSetDefault(view, offset, v1, v2) { view.setBigInt64(offset, v1); view.setBigUint64(offset + 8, v2); }
function testSetVar(view, offset, v1, v2, le) { view.setBigInt64(offset, v1, le); view.setBigUint64(offset + 8, v2, le); }
noInline(testSetLE);
noInline(testSetBE);
noInline(testSetDefault);
noInline(testSetVar);

const SIZE = 64;
const bytes = new Uint8Array(SIZE);
const view = new DataView(bytes.buffer);

const interestingValues = [
    0n, 1n, -1n, 2n, 255n, 256n, 0x7fn,
    0x7fffffffn, 0x80000000n, 0xffffffffn, 0x100000000n,
    (1n << 63n) - 1n, 1n << 63n, (1n << 64n) - 1n,
    -(1n << 63n), -(1n << 63n) + 1n,
    0x0102030405060708n, 0xf1f2f3f4f5f6f7f8n,
    // multi-digit BigInts (wrap modulo 2^64)
    (1n << 64n) + 5n, (1n << 128n) + 7n, -((1n << 64n) + 5n), -((1n << 100n) + 123n),
    (1n << 64n), -(1n << 64n),
];

function toInt64(v) {
    let u = ((v % (1n << 64n)) + (1n << 64n)) % (1n << 64n);
    return u >= (1n << 63n) ? u - (1n << 64n) : u;
}
function toUint64(v) {
    return ((v % (1n << 64n)) + (1n << 64n)) % (1n << 64n);
}

const ITERATIONS = 20000;
for (let i = 0; i < ITERATIONS; i++) {
    const value = interestingValues[i % interestingValues.length];
    const offset = (i * 3) % (SIZE - 16);

    for (const le of [true, false]) {
        // set via intrinsic path, check bytes against reference
        testSetVar(view, offset, value, value, le);
        const expected = new Uint8Array(16);
        refSetBigUint64(expected, 0, value, le);
        refSetBigUint64(expected, 8, value, le);
        for (let j = 0; j < 16; j++)
            assert(bytes[offset + j] === expected[j], `setVar le=${le} value=${value} byte ${j}: ${bytes[offset + j]} != ${expected[j]}`);

        // get via intrinsic path, check against reference
        const [i64v, u64v] = testGetVar(view, offset, le);
        assert(i64v === toInt64(value), `getVar i64 le=${le} value=${value}: ${i64v}`);
        assert(u64v === toUint64(value), `getVar u64 le=${le} value=${value}: ${u64v}`);
    }

    // constant-endianness paths
    testSetLE(view, offset, value, value);
    let [a, b] = testGetLE(view, offset);
    assert(a === toInt64(value), `LE i64 ${value}: ${a}`);
    assert(b === toUint64(value), `LE u64 ${value}: ${b}`);

    testSetBE(view, offset, value, value);
    [a, b] = testGetBE(view, offset);
    assert(a === toInt64(value), `BE i64 ${value}: ${a}`);
    assert(b === toUint64(value), `BE u64 ${value}: ${b}`);

    // default (big-endian per spec)
    testSetDefault(view, offset, value, value);
    [a, b] = testGetDefault(view, offset);
    assert(a === toInt64(value), `default i64 ${value}: ${a}`);
    assert(b === toUint64(value), `default u64 ${value}: ${b}`);
    assert(view.getBigUint64(offset, false) === toUint64(value), `default is big-endian ${value}`);
}

// cross-check against two-uint32 decomposition
for (let i = 0; i < ITERATIONS; i++) {
    const value = interestingValues[i % interestingValues.length];
    testSetLE(view, 0, value, value);
    const lo = BigInt(view.getUint32(0, true));
    const hi = BigInt(view.getUint32(4, true));
    assert(((hi << 32n) | lo) === toUint64(value), `uint32 cross-check ${value}`);
}

// --- out-of-bounds must throw RangeError even once optimized ---
function oobGet(view, offset) { return view.getBigUint64(offset, true); }
function oobSet(view, offset) { view.setBigUint64(offset, 1n, true); }
noInline(oobGet);
noInline(oobSet);
for (let i = 0; i < ITERATIONS; i++) {
    oobGet(view, 0);
    oobSet(view, 0);
}
for (const badOffset of [SIZE - 7, SIZE, -1, 0x7fffffff]) {
    let threw = false;
    try { oobGet(view, badOffset); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, `getBigUint64(${badOffset}) should throw RangeError`);
    threw = false;
    try { oobSet(view, badOffset); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, `setBigUint64(${badOffset}) should throw RangeError`);
}

// --- non-BigInt value to set must throw TypeError after optimization ---
function setAny(view, offset, v) { view.setBigUint64(offset, v, true); }
noInline(setAny);
for (let i = 0; i < ITERATIONS; i++)
    setAny(view, 0, 42n);
for (const bad of [42, 1.5, null, undefined, Symbol("x")]) {
    let threw = false;
    try { setAny(view, 0, bad); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, `setBigUint64 with ${String(bad)} should throw TypeError`);
}
{
    let threw = false;
    try { setAny(view, 0, {}); } catch (e) { threw = e instanceof SyntaxError; }
    assert(threw, "setBigUint64 with {} should throw SyntaxError");
}
// string and boolean convert via ToBigInt without throwing
setAny(view, 0, "42");
assert(view.getBigUint64(0, true) === 42n, "string value converts");
setAny(view, 0, true);
assert(view.getBigUint64(0, true) === 1n, "boolean value converts");
// after the exits, bigint values must still work
setAny(view, 0, 7n);
assert(view.getBigUint64(0, true) === 7n, "set after exits");

// --- number value to get offset coercion & detached buffer ---
{
    const buf = new ArrayBuffer(16);
    const v = new DataView(buf);
    v.setBigUint64(0, 0x1122334455667788n, true);
    transferArrayBuffer(buf);
    let threw = false;
    try { oobGet(v, 0); } catch (e) { threw = e instanceof TypeError || e instanceof RangeError; }
    assert(threw, "get on detached buffer should throw");
    threw = false;
    try { oobSet(v, 0); } catch (e) { threw = e instanceof TypeError || e instanceof RangeError; }
    assert(threw, "set on detached buffer should throw");
}

// --- resizable ArrayBuffer ---
{
    const rab = new ArrayBuffer(32, { maxByteLength: 64 });
    const v = new DataView(rab);
    function rget(v, o) { return v.getBigUint64(o, true); }
    function rset(v, o, val) { v.setBigUint64(o, val, true); }
    noInline(rget);
    noInline(rset);
    for (let i = 0; i < ITERATIONS; i++) {
        rset(v, 16, BigInt(i));
        assert(rget(v, 16) === BigInt(i), "resizable basic");
    }
    rab.resize(16);
    let threw = false;
    try { rget(v, 16); } catch (e) { threw = true; }
    assert(threw, "get past shrunk resizable buffer should throw");
    rab.resize(64);
    rset(v, 48, 99n);
    assert(rget(v, 48) === 99n, "grown resizable buffer");
}

// --- GC stress: results are real, independent BigInts ---
function gcGet(view, offset) { return view.getBigUint64(offset, true); }
noInline(gcGet);
view.setBigUint64(0, 0xdeadbeefcafebaben, true);
{
    const keep = [];
    for (let i = 0; i < 50000; i++) {
        keep.push(gcGet(view, 0));
        if (keep.length > 64)
            keep.shift();
        if ((i % 10000) === 0)
            fullGC();
    }
    for (const k of keep)
        assert(k === 0xdeadbeefcafebaben, "gc stress value");
}

// --- negative zero-adjacent and zero BigInt handling in set fast path ---
function setZero(view) { view.setBigUint64(0, 0n, true); view.setBigInt64(8, 0n, true); }
noInline(setZero);
view.setBigUint64(0, ~0n & ((1n << 64n) - 1n), true);
view.setBigUint64(8, ~0n & ((1n << 64n) - 1n), true);
for (let i = 0; i < ITERATIONS; i++)
    setZero(view);
assert(view.getBigUint64(0, true) === 0n, "zero set u64");
assert(view.getBigInt64(8, true) === 0n, "zero set i64");

print("PASS");
