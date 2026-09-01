//@ requireOptions("--useJSThreads=1")
// ThreadLocal: per-thread storage slots. The ThreadLocal object itself is
// shared; .value reads and writes are isolated per thread.
load("../resources/assert.js", "caller relative");

shouldThrow(TypeError, () => ThreadLocal());

const tls = new ThreadLocal();

// Default value is undefined.
shouldBe(tls.value, undefined);

// Basic set/get on the main thread, including falsy and object values.
tls.value = 0;
shouldBe(tls.value, 0);
tls.value = "main";
shouldBe(tls.value, "main");
const mainObject = { from: "main" };
tls.value = mainObject;
shouldBe(tls.value, mainObject);
tls.value = "main";

// A fresh thread starts with undefined, can set its own value, and never
// disturbs other threads' slots.
const t1 = new Thread(() => {
    shouldBe(tls.value, undefined, "child thread does not inherit the parent's value");
    tls.value = "worker-1";
    shouldBe(tls.value, "worker-1");
    return tls.value;
});
shouldBe(t1.join(), "worker-1");
shouldBe(tls.value, "main", "main thread's slot untouched by t1");

// Many threads racing on the same ThreadLocal each see only their own slot.
const THREADS = 4;
const LAPS = 100;
const workers = spawnN(THREADS, index => {
    shouldBe(tls.value, undefined);
    for (let i = 0; i < LAPS; ++i) {
        tls.value = index * 1000 + i;
        // Re-read through a separate function call to make sure the slot is
        // genuinely per-thread state, not call-local.
        const read = (() => tls.value)();
        if (read !== index * 1000 + i)
            throw new Error("thread " + index + " saw foreign value " + read);
    }
    tls.value = { owner: index };
    return tls.value.owner;
});
const owners = joinAll(workers);
for (let i = 0; i < THREADS; ++i)
    shouldBe(owners[i], i);
shouldBe(tls.value, "main", "main thread's slot survives the stampede");

// Two ThreadLocals are independent slots, in every thread.
const tlsA = new ThreadLocal();
const tlsB = new ThreadLocal();
tlsA.value = "A-main";
tlsB.value = "B-main";
const t2 = new Thread(() => {
    shouldBe(tlsA.value, undefined);
    shouldBe(tlsB.value, undefined);
    tlsA.value = "A-worker";
    shouldBe(tlsA.value, "A-worker");
    shouldBe(tlsB.value, undefined, "sibling ThreadLocal unaffected");
    return tlsA.value;
});
shouldBe(t2.join(), "A-worker");
shouldBe(tlsA.value, "A-main");
shouldBe(tlsB.value, "B-main");

// A ThreadLocal created inside a worker is a shared object like any other,
// but its main-thread slot is empty.
const made = new Thread(() => {
    const inner = new ThreadLocal();
    inner.value = "set-in-worker";
    return { inner, seen: inner.value };
}).join();
shouldBe(made.seen, "set-in-worker");
shouldBeTrue(made.inner instanceof ThreadLocal);
shouldBe(made.inner.value, undefined, "main thread sees its own empty slot");

// Values are not deep-copied: threads share the object stored by another
// thread only if they pass it explicitly (the slot itself stays private).
const shared = { hits: 0 };
tls.value = shared;
const t3 = new Thread(payload => {
    shouldBe(tls.value, undefined, "slot is private even though the payload object is shared");
    payload.hits++;
    return payload === shared;
}, shared);
shouldBeTrue(t3.join());
shouldBe(tls.value, shared);
shouldBe(shared.hits, 1);

// The classic strawman example from the design doc.
const threadLocal = new ThreadLocal();
function foo() {
    return threadLocal.value;
}
threadLocal.value = 42;
const strawman = new Thread(() => {
    threadLocal.value = 43;
    return foo();
});
shouldBe(strawman.join(), 43);
shouldBe(foo(), 42);
