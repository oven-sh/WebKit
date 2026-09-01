//@ requireOptions("--useJSThreads=1")
// Functional coverage of the typed-array C++ paths that dereference a single
// base-pointer snapshot taken after their length re-check
// (%TypedArray%.prototype.{includes,indexOf,lastIndexOf,reverse,sort}): every
// path must keep its flag-off semantics, including the detached TypeError, the
// fromIndex coercion detaching or shrinking the buffer, and a sort comparator
// that detaches the buffer under the pending write-back. Each block runs on the
// main thread and again on spawned Threads.
load("../harness.js", "caller relative");

function checkSearch() {
    const ints = new Int32Array([5, -1, 3, 0, 3]);
    shouldBe(ints.includes(3), true, "includes int32");
    shouldBe(ints.includes(3, 3), true, "includes from index");
    shouldBe(ints.includes(7), false, "includes missing");
    shouldBe(ints.includes(undefined), false, "includes undefined");
    shouldBe(ints.indexOf(3), 2, "indexOf int32");
    shouldBe(ints.indexOf(3, 3), 4, "indexOf from index");
    shouldBe(ints.indexOf(7), -1, "indexOf missing");
    shouldBe(ints.lastIndexOf(3), 4, "lastIndexOf int32");
    shouldBe(ints.lastIndexOf(3, 3), 2, "lastIndexOf from index");
    shouldBe(ints.lastIndexOf(3, -3), 2, "lastIndexOf negative from index");
    shouldBe(ints.lastIndexOf(7), -1, "lastIndexOf missing");

    const bytes = new Uint8Array([0, 255, 7, 255]);
    shouldBe(bytes.includes(255), true, "includes uint8");
    shouldBe(bytes.indexOf(255), 1, "indexOf uint8");
    shouldBe(bytes.lastIndexOf(255), 3, "lastIndexOf uint8");
    shouldBe(bytes.indexOf(256), -1, "indexOf out of range value");

    const shorts = new Int16Array([-2, 9, -2]);
    shouldBe(shorts.indexOf(-2), 0, "indexOf int16");
    shouldBe(shorts.lastIndexOf(-2), 2, "lastIndexOf int16");

    const floats = new Float64Array([1.5, NaN, -0, 2.5]);
    shouldBe(floats.includes(NaN), true, "includes NaN");
    shouldBe(floats.indexOf(NaN), -1, "indexOf NaN");
    shouldBe(floats.lastIndexOf(NaN), -1, "lastIndexOf NaN");
    shouldBe(floats.includes(0), true, "includes -0 as 0");
    shouldBe(floats.indexOf(0), 2, "indexOf -0 as 0");
    shouldBe(floats.lastIndexOf(2.5), 3, "lastIndexOf float64");
    const singles = new Float32Array([0.5, NaN]);
    shouldBe(singles.includes(NaN), true, "includes NaN float32");
    shouldBe(singles.indexOf(0.5), 0, "indexOf float32");
    const halves = new Float16Array([0.25, NaN, 0.25]);
    shouldBe(halves.includes(NaN), true, "includes NaN float16");
    shouldBe(halves.lastIndexOf(0.25), 2, "lastIndexOf float16");

    const bigs = new BigInt64Array([3n, -7n, 3n]);
    shouldBe(bigs.includes(-7n), true, "includes bigint");
    shouldBe(bigs.indexOf(3n), 0, "indexOf bigint");
    shouldBe(bigs.lastIndexOf(3n), 2, "lastIndexOf bigint");
    shouldBe(bigs.includes(3), false, "includes number in bigint array");

    // The fromIndex coercion runs user code before the length re-check.
    const resizable = new ArrayBuffer(16, { maxByteLength: 32 });
    const tracking = new Uint32Array(resizable);
    tracking.set([1, 2, 3, 4]);
    shouldBe(tracking.includes(4, { valueOf() { resizable.resize(8); return 0; } }), false, "includes after shrink in fromIndex");
    resizable.resize(16);
    tracking.set([1, 2, 3, 4]);
    shouldBe(tracking.indexOf(4, { valueOf() { resizable.resize(8); return 0; } }), -1, "indexOf after shrink in fromIndex");
    resizable.resize(16);
    tracking.set([1, 2, 3, 4]);
    shouldBe(tracking.lastIndexOf(4, { valueOf() { resizable.resize(8); return 3; } }), -1, "lastIndexOf after shrink in fromIndex");
    resizable.resize(16);
    tracking.set([1, 2, 3, 4]);
    shouldBe(tracking.lastIndexOf(2, { valueOf() { resizable.resize(8); return 3; } }), 1, "lastIndexOf in bounds after shrink in fromIndex");

    const detachable = new ArrayBuffer(8);
    const detaching = new Int16Array(detachable);
    detaching.set([1, 2, 3, 4]);
    shouldBe(detaching.includes(undefined, { valueOf() { detachable.transfer(); return 1; } }), true, "includes undefined after detach in fromIndex");
    shouldThrow(TypeError, () => detaching.includes(1)); // includes on detached
    shouldThrow(TypeError, () => detaching.indexOf(1)); // indexOf on detached
    shouldThrow(TypeError, () => detaching.lastIndexOf(1)); // lastIndexOf on detached

    const detachable2 = new ArrayBuffer(8);
    const detaching2 = new Int16Array(detachable2);
    detaching2.set([1, 2, 3, 4]);
    shouldBe(detaching2.indexOf(1, { valueOf() { detachable2.transfer(); return 0; } }), -1, "indexOf after detach in fromIndex");
    const detachable3 = new ArrayBuffer(8);
    const detaching3 = new Int16Array(detachable3);
    detaching3.set([1, 2, 3, 4]);
    shouldBe(detaching3.lastIndexOf(1, { valueOf() { detachable3.transfer(); return 3; } }), -1, "lastIndexOf after detach in fromIndex");
}

function checkReverse() {
    const ints = new Int32Array([1, 2, 3, 4, 5]);
    shouldBe(ints.reverse(), ints, "reverse returns this");
    shouldBe(ints.join(), "5,4,3,2,1", "reverse odd length");
    const bytes = new Uint8Array([1, 2, 3, 4]);
    bytes.reverse();
    shouldBe(bytes.join(), "4,3,2,1", "reverse even length");
    const single = new Float64Array([2.5]);
    single.reverse();
    shouldBe(single.join(), "2.5", "reverse single element");
    const empty = new Int8Array(0);
    empty.reverse();
    shouldBe(empty.length, 0, "reverse empty");
    const halves = new Float16Array([1, -1, 0.5]);
    halves.reverse();
    shouldBe(halves.join(), "0.5,-1,1", "reverse float16");
    const bigs = new BigUint64Array([1n, 2n, 3n]);
    bigs.reverse();
    shouldBe(bigs.join(), "3,2,1", "reverse bigint");
    const sub = new Uint16Array([9, 1, 2, 3, 9]).subarray(1, 4);
    sub.reverse();
    shouldBe(sub.join(), "3,2,1", "reverse subarray");

    const resizable = new ArrayBuffer(16, { maxByteLength: 32 });
    const tracking = new Uint32Array(resizable);
    tracking.set([1, 2, 3, 4]);
    resizable.resize(8);
    tracking.reverse();
    shouldBe(tracking.join(), "2,1", "reverse after shrink");

    const detachable = new ArrayBuffer(8);
    const detached = new Int16Array(detachable);
    detachable.transfer();
    shouldThrow(TypeError, () => detached.reverse()); // reverse on detached
}

function checkSortWithComparator() {
    const ints = new Int32Array([5, -1, 3, 0, 2]);
    shouldBe(ints.sort((a, b) => b - a), ints, "sort with comparator returns this");
    shouldBe(ints.join(), "5,3,2,0,-1", "sort descending");
    const floats = new Float64Array([2.5, -3, 0.5]);
    floats.sort((a, b) => a - b);
    shouldBe(floats.join(), "-3,0.5,2.5", "sort float64 with comparator");
    const bigs = new BigInt64Array([3n, -7n, 1n]);
    bigs.sort((a, b) => (a < b ? 1 : a > b ? -1 : 0));
    shouldBe(bigs.join(), "3,1,-7", "sort bigint with comparator");
    const single = new Uint8Array([7]);
    single.sort(() => { throw new Error("comparator must not run for one element"); });
    shouldBe(single.join(), "7", "sort single element with comparator");
    const sorted = new Uint8Array([3, 1, 2]).toSorted((a, b) => a - b);
    shouldBe(sorted.join(), "1,2,3", "toSorted with comparator");

    // The comparator detaches the buffer: the sort completes without writing back.
    const detachable = new ArrayBuffer(16);
    const detaching = new Int32Array(detachable);
    detaching.set([4, 3, 2, 1]);
    let calls = 0;
    shouldBe(detaching.sort((a, b) => { if (++calls === 1) detachable.transfer(); return a - b; }), detaching, "sort returns this after detach in comparator");
    shouldBe(calls > 0, true, "comparator ran");
    shouldBe(detaching.length, 0, "sort left the view detached");

    // The comparator shrinks a resizable buffer: only the in-bounds prefix is written back.
    const resizable = new ArrayBuffer(16, { maxByteLength: 32 });
    const tracking = new Int32Array(resizable);
    tracking.set([4, 3, 2, 1]);
    calls = 0;
    tracking.sort((a, b) => { if (++calls === 1) resizable.resize(8); return a - b; });
    shouldBe(tracking.join(), "1,2", "sort after shrink in comparator");

    // The comparator turns a fast view wasteful; the write-back must use the relocated backing store.
    const turning = new Uint16Array([3, 1, 2]);
    calls = 0;
    turning.sort((a, b) => { if (++calls === 1) turning.buffer; return a - b; });
    shouldBe(turning.join(), "1,2,3", "sort after wasteful transition in comparator");

    const detached = new Uint16Array(new ArrayBuffer(8));
    detached.buffer.transfer();
    shouldThrow(TypeError, () => detached.sort((a, b) => a - b)); // sort with comparator on detached
}

function runAll() {
    checkSearch();
    checkReverse();
    checkSortWithComparator();
}

runAll();
joinAll(spawnN(2, runAll));
runAll();
