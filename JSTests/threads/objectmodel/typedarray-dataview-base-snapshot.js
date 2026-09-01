//@ requireOptions("--useJSThreads=1")
// Functional coverage of the typed-array and DataView C++ paths that take a
// single base-pointer snapshot before their length proof (DataView get/set,
// %TypedArray%.prototype.{set,copyWithin,fill,sort,forEach}): every path must
// keep its flag-off semantics, including the detached TypeError, the
// out-of-bounds RangeError, and forEach observing a detach performed by its own
// callback. Each block runs on the main thread and again on a spawned Thread.
load("../harness.js", "caller relative");

function checkDataView() {
    const buffer = new ArrayBuffer(16);
    const view = new DataView(buffer);

    view.setUint8(0, 0xab);
    shouldBe(view.getUint8(0), 0xab, "u8");
    view.setInt8(1, -5);
    shouldBe(view.getInt8(1), -5, "i8");

    view.setUint16(2, 0x1234);
    shouldBe(view.getUint16(2), 0x1234, "u16 big-endian");
    shouldBe(view.getUint16(2, true), 0x3412, "u16 read little-endian");
    view.setUint16(2, 0x1234, true);
    shouldBe(view.getUint16(2, true), 0x1234, "u16 little-endian");
    shouldBe(view.getUint8(2), 0x34, "u16 little-endian low byte first");

    view.setInt32(4, -123456789);
    shouldBe(view.getInt32(4), -123456789, "i32");
    view.setUint32(4, 0xdeadbeef, true);
    shouldBe(view.getUint32(4, true), 0xdeadbeef, "u32 little-endian");
    shouldBe(view.getUint32(4), 0xefbeadde, "u32 byte-swapped read");

    view.setFloat32(8, 1.5);
    shouldBe(view.getFloat32(8), 1.5, "f32");
    view.setFloat64(8, Math.PI, true);
    shouldBe(view.getFloat64(8, true), Math.PI, "f64 little-endian");
    view.setFloat16(0, 0.5);
    shouldBe(view.getFloat16(0), 0.5, "f16");

    view.setBigInt64(8, -42n);
    shouldBe(view.getBigInt64(8), -42n, "i64");
    view.setBigUint64(8, 0x0102030405060708n, true);
    shouldBe(view.getBigUint64(8, true), 0x0102030405060708n, "u64 little-endian");
    shouldBe(view.getUint8(8), 0x08, "u64 little-endian low byte first");

    // Unaligned offsets go through the byte loop too.
    view.setUint32(1, 0x01020304);
    shouldBe(view.getUint32(1), 0x01020304, "unaligned u32");
    shouldBe(view.getUint8(1), 0x01, "unaligned u32 high byte first");

    shouldThrow(RangeError, () => view.getUint32(13)); // u32 past the end
    shouldThrow(RangeError, () => view.setFloat64(9, 1)); // f64 past the end
    shouldThrow(RangeError, () => view.getUint8(16)); // u8 at the end

    const resizable = new ArrayBuffer(8, { maxByteLength: 16 });
    const tracking = new DataView(resizable);
    tracking.setUint32(4, 7);
    shouldBe(tracking.getUint32(4), 7, "resizable u32");
    resizable.resize(4);
    shouldThrow(RangeError, () => tracking.getUint32(4)); // resizable shrunk out of bounds
    resizable.resize(16);
    shouldBe(tracking.getUint32(12), 0, "resizable regrown reads zero");

    buffer.transfer();
    shouldThrow(TypeError, () => view.getUint8(0)); // get on detached
    shouldThrow(TypeError, () => view.setUint8(0, 1)); // set on detached
    shouldThrow(TypeError, () => view.getFloat64(0)); // f64 get on detached
}

function checkSetCopyWithinFill() {
    const source = new Int32Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const target = new Int32Array(8);
    target.set(source);
    shouldBe(target.join(), "1,2,3,4,5,6,7,8", "set same type");
    target.set(new Int32Array([9, 9]), 6);
    shouldBe(target.join(), "1,2,3,4,5,6,9,9", "set same type at offset");
    target.set(new Uint32Array([11, 12]), 0);
    shouldBe(target.join(), "11,12,3,4,5,6,9,9", "set same-size integer type");
    target.set(target.subarray(0, 4), 2);
    shouldBe(target.join(), "11,12,11,12,3,4,9,9", "set overlapping forward");
    target.set(target.subarray(2, 6), 0);
    shouldBe(target.join(), "11,12,3,4,3,4,9,9", "set overlapping backward");
    target.set([1.5, 2.5], 0);
    shouldBe(target.join(), "1,2,3,4,3,4,9,9", "set from array");
    target.set(new Float64Array([7.9, 8.9]), 6);
    shouldBe(target.join(), "1,2,3,4,3,4,7,8", "set converting type");
    shouldThrow(RangeError, () => target.set(source, 1)); // set out of range

    const copy = new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7]);
    copy.copyWithin(2, 0, 4);
    shouldBe(copy.join(), "0,1,0,1,2,3,6,7", "copyWithin forward overlap");
    copy.copyWithin(0, 2, 6);
    shouldBe(copy.join(), "0,1,2,3,2,3,6,7", "copyWithin backward overlap");
    copy.copyWithin(6, 0);
    shouldBe(copy.join(), "0,1,2,3,2,3,0,1", "copyWithin clamped count");

    const filled = new Float64Array(6);
    filled.fill(2.5);
    shouldBe(filled.join(), "2.5,2.5,2.5,2.5,2.5,2.5", "fill all");
    filled.fill(-1, 1, 3);
    shouldBe(filled.join(), "2.5,-1,-1,2.5,2.5,2.5", "fill range");
    filled.fill(9, 4);
    shouldBe(filled.join(), "2.5,-1,-1,2.5,9,9", "fill tail");
    const bytes = new Uint8Array(4);
    bytes.fill(0x7f);
    shouldBe(bytes.join(), "127,127,127,127", "fill bytes");
    const words = new Uint32Array(3);
    words.fill(0xffffffff, 1);
    shouldBe(words.join(), "0,4294967295,4294967295", "fill words");

    const resizable = new ArrayBuffer(32, { maxByteLength: 64 });
    const tracking = new Int32Array(resizable);
    tracking.fill(3);
    resizable.resize(16);
    tracking.copyWithin(1, 0, 4);
    shouldBe(tracking.join(), "3,3,3,3", "copyWithin after shrink");
    tracking.fill(5, 2, 8);
    shouldBe(tracking.join(), "3,3,5,5", "fill after shrink");

    const detachable = new ArrayBuffer(16);
    const detached = new Int32Array(detachable);
    detachable.transfer();
    shouldThrow(TypeError, () => detached.fill(1)); // fill on detached
    shouldThrow(TypeError, () => detached.copyWithin(0, 1)); // copyWithin on detached
    shouldThrow(TypeError, () => detached.set([1])); // set on detached
    shouldThrow(TypeError, () => new Int32Array(4).set(detached)); // set from detached
}

function checkSort() {
    const ints = new Int32Array([5, -1, 3, 0, 2]);
    ints.sort();
    shouldBe(ints.join(), "-1,0,2,3,5", "sort int32");
    const floats = new Float64Array([2.5, -0, 0, NaN, -3, Infinity]);
    floats.sort();
    shouldBe(Object.is(floats[1], -0) && Object.is(floats[2], 0), true, "sort -0 before +0");
    shouldBe(floats[0], -3, "sort float min");
    shouldBe(floats[4], Infinity, "sort float infinity");
    shouldBe(Number.isNaN(floats[5]), true, "sort float NaN last");
    const halves = new Float16Array([1, -1, 0.5]);
    halves.sort();
    shouldBe(halves.join(), "-1,0.5,1", "sort float16");
    const shared = new Uint8Array(new SharedArrayBuffer(4));
    shared.set([3, 1, 2, 0]);
    shared.sort();
    shouldBe(shared.join(), "0,1,2,3", "sort shared");
    const bigs = new BigInt64Array([3n, -7n, 1n]);
    bigs.sort();
    shouldBe(bigs.join(), "-7,1,3", "sort bigint");
    const withComparator = new Uint16Array([1, 3, 2]);
    withComparator.sort((a, b) => b - a);
    shouldBe(withComparator.join(), "3,2,1", "sort with comparator");

    const detachable = new ArrayBuffer(8);
    const detached = new Uint16Array(detachable);
    detachable.transfer();
    shouldThrow(TypeError, () => detached.sort()); // sort on detached
}

function checkForEach() {
    // Fast view without an ArrayBuffer.
    const fast = new Int16Array([4, 5, 6]);
    let seen = [];
    fast.forEach((value, index) => { seen.push(index + ":" + value); });
    shouldBe(seen.join(), "0:4,1:5,2:6", "forEach fast view");

    // Wasteful view over an ArrayBuffer; the callback detaches it mid-iteration
    // and the remaining elements must read as undefined.
    const buffer = new ArrayBuffer(16);
    const wasteful = new Int32Array(buffer);
    wasteful.set([1, 2, 3, 4]);
    seen = [];
    wasteful.forEach((value, index) => {
        seen.push(String(value));
        if (index === 1)
            buffer.transfer();
    });
    shouldBe(seen.join(), "1,2,undefined,undefined", "forEach detach in callback");

    // Fast view turned wasteful by .buffer inside the callback, then detached.
    const turning = new Float32Array([1.5, 2.5, 3.5]);
    seen = [];
    turning.forEach((value, index) => {
        seen.push(String(value));
        if (index === 0)
            turning.buffer.transfer();
    });
    shouldBe(seen.join(), "1.5,undefined,undefined", "forEach wasteful transition then detach");

    // Resizable view; the callback shrinks the buffer so later indices fall out
    // of bounds, then regrows it.
    const resizable = new ArrayBuffer(16, { maxByteLength: 32 });
    const tracking = new Uint32Array(resizable);
    tracking.set([10, 20, 30, 40]);
    seen = [];
    tracking.forEach((value, index) => {
        seen.push(String(value));
        if (index === 0)
            resizable.resize(8);
        if (index === 2)
            resizable.resize(16);
    });
    shouldBe(seen.join(), "10,20,undefined,0", "forEach resize in callback");

    // Resizable view detached by the callback.
    const resizableDetach = new ArrayBuffer(8, { maxByteLength: 16 });
    const trackingDetach = new Uint8Array(resizableDetach);
    trackingDetach.set([1, 2, 3, 4, 5, 6, 7, 8]);
    seen = [];
    let count = 0;
    trackingDetach.forEach((value, index) => {
        count++;
        seen.push(String(value));
        if (index === 5)
            resizableDetach.transfer();
    });
    shouldBe(count, 8, "forEach visits every original index after detach");
    shouldBe(seen.join(), "1,2,3,4,5,6,undefined,undefined", "forEach resizable detach in callback");

    // Other forEach-family functions share the same loop.
    shouldBe(new Int8Array([1, 2, 3]).some(v => v === 2), true, "some");
    shouldBe(new Int8Array([1, 2, 3]).every(v => v > 0), true, "every");
    shouldBe(new Int8Array([1, 2, 3]).findLast(v => v < 3), 2, "findLast");
    shouldBe(new Int8Array([1, 2, 3]).findLastIndex(v => v === 1), 0, "findLastIndex");
}

function runAll() {
    checkDataView();
    checkSetCopyWithinFill();
    checkSort();
    checkForEach();
}

runAll();
joinAll(spawnN(2, runAll));
runAll();
