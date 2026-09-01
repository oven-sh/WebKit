//@ requireOptions("--useJSThreads=1")
// Thread.restrict: receiver validation, owner-side semantics, and
// cross-thread restriction attempts (ConcurrentAccessError).
load("../resources/assert.js", "caller relative");

// Non-objects are rejected with TypeError.
shouldThrow(TypeError, () => Thread.restrict());
shouldThrow(TypeError, () => Thread.restrict(undefined));
shouldThrow(TypeError, () => Thread.restrict(null));
shouldThrow(TypeError, () => Thread.restrict(1));
shouldThrow(TypeError, () => Thread.restrict("string"));
shouldThrow(TypeError, () => Thread.restrict(true));
shouldThrow(TypeError, () => Thread.restrict(Symbol()));
shouldThrow(TypeError, () => Thread.restrict(123n));

// Excluded receivers are rejected with TypeError.
shouldThrow(TypeError, () => Thread.restrict(globalThis));
shouldThrow(TypeError, () => Thread.restrict(Array.prototype));
// NOTE: Object.prototype is deliberately NOT here — the Object pair is not
// in Dev 8's frozen species-protected list and ObjectPrototype has no
// enforced method-table overrides, so restricting it is legal
// (catastrophic-but-perf-only per 5.7.1; recorded round-3 delta, reaffirmed
// under D13, docs/threads/INTEGRATE-api.md). We do not actually restrict
// it: that would poison every later test in this VM.
shouldThrow(TypeError, () => Thread.restrict(RegExp.prototype));
shouldThrow(TypeError, () => Thread.restrict(Promise.prototype));
shouldThrow(TypeError, () => Thread.restrict(new Proxy({}, {})));

// Restricting returns the same object, and the owner's access is unchanged.
const o = { secret: 1 };
shouldBe(Thread.restrict(o), o);
shouldBe(o.secret, 1);
o.secret = 2;
shouldBe(o.secret, 2);
o.added = "later";
shouldBe(o.added, "later");
delete o.added;
shouldBeFalse("added" in o);
shouldBe(Object.keys(o).length, 1);

// Idempotent when re-restricted by the owner.
shouldBe(Thread.restrict(o), o);

// Arrays can be restricted; owner-side indexed access still works.
const arr = Thread.restrict([1, 2, 3]);
shouldBe(arr[0], 1);
arr.push(4);
shouldBe(arr.length, 4);
shouldBe(arr[3], 4);

// ConcurrentAccessError is exposed as a global Error subclass.
shouldBe(typeof ConcurrentAccessError, "function");
const cae = new ConcurrentAccessError("msg");
shouldBeTrue(cae instanceof ConcurrentAccessError);
shouldBeTrue(cae instanceof Error);
shouldBe(cae.name, "ConcurrentAccessError");
shouldBe(cae.message, "msg");

// A foreign thread cannot re-restrict an object owned by the main thread.
const foreignRestrict = new Thread(target => {
    try {
        Thread.restrict(target);
        return "no-throw";
    } catch (e) {
        return e.name + ":" + (e instanceof ConcurrentAccessError);
    }
}, o);
shouldBe(foreignRestrict.join(), "ConcurrentAccessError:true");

// A spawned thread can restrict an object it owns; the main thread then
// cannot restrict it.
const fromWorker = new Thread(() => {
    const mine = { worker: true };
    Thread.restrict(mine);
    shouldBe(Thread.restrict(mine), mine); // idempotent for its owner
    return mine;
}).join();
shouldThrow(ConcurrentAccessError, () => Thread.restrict(fromWorker));

// Two distinct spawned threads: the second cannot restrict the first's
// object either (foreign != just "not main").
const ownerThread = new Thread(() => Thread.restrict({ tag: "owned" }));
const ownedObject = ownerThread.join();
const otherThread = new Thread(target => {
    try {
        Thread.restrict(target);
        return "no-throw";
    } catch (e) {
        return e.name;
    }
}, ownedObject);
shouldBe(otherThread.join(), "ConcurrentAccessError");

// Restriction survives the owner thread's death: ownership does not
// silently transfer to whoever asks next.
shouldThrow(ConcurrentAccessError, () => Thread.restrict(ownedObject));
