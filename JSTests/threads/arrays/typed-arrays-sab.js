//@ requireOptions("--useJSThreads=1")
// Typed arrays and SharedArrayBuffer interop with Thread(): SAB memory shared
// via captured scope, per-thread views, Atomics on SAB elements vs. Atomics on
// object properties, and plain (non-shared) ArrayBuffer views shared as
// ordinary heap objects.
load("../resources/assert.js", "caller relative");

// --- A view created on one thread is readable/writable from another ---

const sab = new SharedArrayBuffer(64);
const i32 = new Int32Array(sab);
i32[0] = 42;
shouldBe(new Thread(view => view[0], i32).join(), 42);
new Thread(view => { view[1] = 7; }, i32).join();
shouldBe(i32[1], 7);

// --- A foreign thread can create its own view over the same SAB ---

shouldBe(new Thread(buffer => {
    const view = new Int32Array(buffer);
    view[2] = 1234;
    return view[0];
}, sab).join(), 42);
shouldBe(i32[2], 1234);

// Different element types over the same memory.
new Thread(buffer => { new Uint8Array(buffer)[12] = 0xff; }, sab).join();
shouldBe(i32[3], 0xff);
const f64 = new Float64Array(sab, 32, 2);
new Thread(view => { view[0] = 0.5; }, f64).join();
shouldBe(f64[0], 0.5);

// DataView across threads.
shouldBe(new Thread(buffer => {
    const dv = new DataView(buffer);
    dv.setInt32(16, 0x01020304, true);
    return dv.getInt32(16, true);
}, sab).join(), 0x01020304);
shouldBe(new DataView(sab).getInt32(16, true), 0x01020304);

// --- Atomic counters on a SAB: exact totals across threads ---

const counterSab = new SharedArrayBuffer(8);
const counter = new Int32Array(counterSab);
joinAll(spawnN(4, () => {
    for (let i = 0; i < 1000; ++i)
        Atomics.add(counter, 0, 1);
}));
shouldBe(Atomics.load(counter, 0), 4000);

// compareExchange-based spinlock-free increment (each thread CASes until it wins).
Atomics.store(counter, 1, 0);
joinAll(spawnN(4, () => {
    for (let i = 0; i < 200; ++i) {
        for (;;) {
            const old = Atomics.load(counter, 1);
            if (Atomics.compareExchange(counter, 1, old, old + 1) === old)
                break;
        }
    }
}));
shouldBe(counter[1], 800);

// --- Atomics.wait/notify on a SAB across threads ---
// The waiter loops on a timed wait so the test cannot hang regardless of how
// the notify interleaves with parking.

const futex = new Int32Array(new SharedArrayBuffer(8));
const waiter = new Thread(view => {
    let result = "never-waited";
    while (Atomics.load(view, 0) === 0)
        result = Atomics.wait(view, 0, 0, 50);
    return result + ":" + Atomics.load(view, 0);
}, futex);
Atomics.store(futex, 0, 1);
Atomics.notify(futex, 0);
const waitOutcome = waiter.join();
shouldBeTrue(
    waitOutcome === "ok:1" || waitOutcome === "not-equal:1" || waitOutcome === "timed-out:1" || waitOutcome === "never-waited:1",
    "unexpected wait outcome: " + waitOutcome);

// --- Object-property Atomics and SAB Atomics interoperate in one program ---

const mailbox = { flag: 0 };
const dataSab = new Int32Array(new SharedArrayBuffer(4));
new Thread((box, data) => {
    Atomics.store(data, 0, 99);
    Atomics.store(box, "flag", 1);
}, mailbox, dataSab).join();
shouldBe(Atomics.load(mailbox, "flag"), 1);
shouldBe(Atomics.load(dataSab, 0), 99);

// --- Plain (non-shared) ArrayBuffer views are still shared heap objects ---

const plain = new Int32Array(new ArrayBuffer(16));
plain[0] = 5;
shouldBe(new Thread(view => { view[1] = view[0] * 2; return view.length; }, plain).join(), 4);
shouldBe(plain[1], 10);
// Atomics on non-shared Int32Array are allowed by the spec.
shouldBe(Atomics.add(plain, 0, 1), 5);
shouldBe(plain[0], 6);

// --- Typed arrays stored as elements of a shared ordinary array ---

const tableSab = new SharedArrayBuffer(16);
const table = [new Int32Array(tableSab, 0, 2), new Int32Array(tableSab, 8, 2)];
joinAll(spawnN(2, index => {
    table[index][0] = index + 1;
    table[index][1] = (index + 1) * 10;
}));
shouldBe(table[0][0], 1);
shouldBe(table[0][1], 10);
shouldBe(table[1][0], 2);
shouldBe(table[1][1], 20);
// Both views alias one SAB; verify via a fresh full-length view.
const flat = new Int32Array(tableSab);
shouldBe(flat[0], 1);
shouldBe(flat[2], 2);

// --- Disjoint-range parallel fill of one large SAB view ---

const big = new Int32Array(new SharedArrayBuffer(4 * 1024));
joinAll(spawnN(4, index => {
    const quarter = big.length / 4;
    for (let i = index * quarter; i < (index + 1) * quarter; ++i)
        big[i] = i;
}));
for (let i = 0; i < big.length; ++i) {
    if (big[i] !== i)
        throw new Error("big[" + i + "] === " + big[i]);
}

// --- Growable SharedArrayBuffer grown by a foreign thread (if supported) ---

let growable = null;
try {
    growable = new SharedArrayBuffer(8, { maxByteLength: 32 });
} catch { /* growable SAB not supported in this build */ }
if (growable && typeof growable.grow === "function") {
    const view = new Int32Array(growable); // length-tracking view
    view[0] = 11;
    new Thread(buffer => { buffer.grow(32); }, growable).join();
    shouldBe(growable.byteLength, 32);
    shouldBe(view.length, 8);
    shouldBe(view[0], 11);
    new Thread(buffer => { new Int32Array(buffer)[7] = 77; }, growable).join();
    shouldBe(view[7], 77);
}

// --- Out-of-bounds and detached-style edge reads from a foreign thread ---

const edge = new Int32Array(new SharedArrayBuffer(8));
shouldBe(new Thread(view => view[100], edge).join(), undefined);
shouldBe(new Thread(view => view[-1], edge).join(), undefined);
new Thread(view => { view[100] = 1; }, edge).join(); // silently ignored
shouldBe(edge.length, 2);
shouldBeFalse(100 in edge);
