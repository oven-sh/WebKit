//@ requireOptions("--useJSThreads=1")
// API-I13: ThreadLocal writes are invisible across threads; the initial
// value is undefined on every thread; any JS value is storable; the value
// accessor lives on ThreadLocal.prototype (4.4/5.8). The 5.8 leak (a dead
// ThreadLocal cell keeps slots in live threads until thread exit) is
// documented, not a violation — untestable from JS and untested here.
load("../harness.js", "caller relative");

const tl = new ThreadLocal();

// initial undefined on the creating (main) thread
shouldBe(tl.value, undefined);

// accessor on the prototype, not the instance
shouldBeTrue(Object.getOwnPropertyDescriptor(ThreadLocal.prototype, "value") !== undefined);
shouldBe(Object.getOwnPropertyDescriptor(tl, "value"), undefined);

// any JS value; reads return what this thread stored, by identity
const mainValue = { main: true };
tl.value = mainValue;
shouldBe(tl.value, mainValue);

// ---- cross-thread isolation ----
shouldBe(new Thread(() => tl.value).join(), undefined,
    "initial value is undefined on a fresh thread despite main's write");
shouldBe(new Thread(() => {
    tl.value = 43;
    return tl.value;
}).join(), 43);
shouldBe(tl.value, mainValue, "spawned thread's write is invisible to main");

// two threads write different values concurrently-ish; each sees its own
{
    const results = joinAll(spawnN(4, which => {
        tl.value = "thread-" + which;
        // re-read after another thread had a chance to run is covered by the
        // join interleaving; the slot must still be ours
        return tl.value;
    }));
    shouldBe(results.join(","), "thread-0,thread-1,thread-2,thread-3");
    shouldBe(tl.value, mainValue);
}

// ---- distinct ThreadLocals are distinct slots ----
{
    const tl2 = new ThreadLocal();
    shouldBe(tl2.value, undefined);
    tl2.value = NaN;
    shouldBeTrue(tl2.value !== tl2.value, "NaN stored and reread");
    shouldBe(tl.value, mainValue, "tl unaffected by tl2");
    tl2.value = -0;
    shouldBe(tl2.value, -0);
    // explicit undefined store: indistinguishable from initial through the
    // accessor, and must not throw
    tl2.value = undefined;
    shouldBe(tl2.value, undefined);
}

// ---- nested threads get fresh slots; overwrite works per thread ----
shouldBe(new Thread(() => {
    tl.value = "outer";
    const innerSaw = new Thread(() => tl.value === undefined ? "fresh" : "stale").join();
    tl.value = "outer2"; // overwrite clears the old Strong (5.10) and replaces
    return innerSaw + ":" + tl.value;
}).join(), "fresh:outer2");
shouldBe(tl.value, mainValue);

// ---- incompatible receiver ----
shouldThrow(TypeError, () => Object.getOwnPropertyDescriptor(ThreadLocal.prototype, "value").get.call({}),
    "ThreadLocal.prototype.value called on incompatible receiver");
