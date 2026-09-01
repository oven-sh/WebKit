//@ requireOptions("--useJSThreads=1")
// FUZZ r3b CoW family regression: §4.8 (I35) materialize-first on the
// non-dictionary locked-transition path.
//
// A CopyOnWrite array literal getting an out-of-line named property add when
// the E4 lock-free predicate is INELIGIBLE (TTL fired by an earlier foreign
// transition / foreign-TID writer / PreciseAllocation) fell into
// trySegmentedTransition's I35 RELEASE_ASSERT (!isCopyOnWrite). E4-eligible
// owner adds were already correct (allocateMoreOutOfLineStorage copies the
// immortal CoW payload into the published growth butterfly). Fix:
// tryPutDirectTransitionConcurrent materialize-first + RESTART before the
// locked protocols, mirroring classifyConcurrentLockedAdd /
// convertToSegmentedButterfly's §4.8-precedes-§4.x ordering. Flag-off
// semantics (Structure.cpp addNewPropertyTransition strips the CoW bit and
// copies the payload) are unchanged.
//
// Minimized from the 12 r3b/triage-r3b CoW repros (all
// `Object.defineProperty(<CoW double literal>, "name", {...})` shapes).
load("../harness.js", "caller relative");

// ---- Owner E4: CoW + named add stays correct (was already; pin it).
{
    const a = [-1.0, 0.5];
    a.named = 42;
    shouldBe(a.named, 42);
    shouldBe(a[0], -1.0);
    shouldBe(a[1], 0.5);
    a[0] = 99; // CoW already materialized by the named add (CoW bit stripped).
    shouldBe(a[0], 99);
}

// ---- Locked path: fire the source TTL set first (foreign-TID transition on
// a sibling literal sharing the same CoW source structure), then the OWNER's
// defineProperty falls past E4 into the locked protocols. Pre-fix this
// tripped the I35 assert; post-fix it materialize-first + RESTARTs.
{
    const sib = [-2.0, 2.5]; // same CopyOnWriteArrayWithDouble source
    new Thread(() => { sib.fired = 1; }).join(); // foreign add: fires source TTL
    for (let r = 0; r < 4; ++r) {
        const a = [-1.0, 0.5];
        Object.defineProperty(a, "acc", {
            configurable: true,
            get() { return this[0] + this[1]; },
            set(v) { this[0] = v; },
        });
        shouldBe(a.acc, -0.5);
        a.acc = 7;
        shouldBe(a[0], 7);
        shouldBe(a[1], 0.5);
        // Further OOL adds (capacity growth on the now-writable source).
        for (let i = 0; i < 12; ++i) a["p" + i] = i;
        for (let i = 0; i < 12; ++i) shouldBe(a["p" + i], i);
        shouldBe(a.length, 2);
    }
}

// ---- Foreign-TID writer: the worker is the first to add a named property
// to an owner-created CoW literal. E4 is ineligible (foreign TID tag), so
// the worker hits the locked-protocols I35 route directly.
{
    for (let r = 0; r < 4; ++r) {
        const a = [3.0, 4.0, 5.0]; // owner-TID CoW
        const t = new Thread(() => {
            Object.defineProperty(a, "constructor", { configurable: true, value: 0xCAFE });
            a.x = 1; a.y = 2; a.z = 3;
            return a[0] + a[1] + a[2];
        });
        shouldBe(t.join(), 12);
        shouldBe(a.constructor, 0xCAFE);
        shouldBe(a.x, 1); shouldBe(a.z, 3);
        shouldBe(a[2], 5.0);
        a[0] = -3;
        shouldBe(a[0], -3);
    }
}

// ---- Int32 + Contiguous CoW shapes (the other two CoW kinds).
{
    const i = [1, 2, 3, 4];
    new Thread(() => { i.tag = "i32"; }).join();
    shouldBe(i.tag, "i32");
    shouldBe(i[3], 4);

    const c = [1, "two", 3];
    new Thread(() => { Object.defineProperty(c, "k", { value: c, configurable: true }); }).join();
    shouldBe(c.k, c);
    shouldBe(c[1], "two");
}
