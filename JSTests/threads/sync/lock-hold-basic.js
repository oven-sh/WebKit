//@ requireOptions("--useJSThreads=1")
// Lock.prototype.hold: single-threaded API surface and invariants.
load("../resources/assert.js", "caller relative");

const lock = new Lock();

// Constructor requires new.
shouldThrow(TypeError, () => Lock());

// hold() requires a Lock receiver.
shouldThrow(TypeError, () => lock.hold.call({}, () => {}));
shouldThrow(TypeError, () => lock.hold.call(null, () => {}));

// hold() requires a callable argument.
shouldThrow(TypeError, () => lock.hold());
shouldThrow(TypeError, () => lock.hold(42));
shouldThrow(TypeError, () => lock.hold("not callable"));

// hold() returns the callback's return value.
shouldBe(lock.hold(() => 42), 42);
shouldBe(lock.hold(() => "str"), "str");
shouldBe(lock.hold(() => undefined), undefined);
const obj = {};
shouldBe(lock.hold(() => obj), obj);

// locked getter reflects hold state.
shouldBeFalse(lock.locked);
lock.hold(() => {
    shouldBeTrue(lock.locked);
});
shouldBeFalse(lock.locked);

// locked getter requires a Lock receiver.
shouldThrow(TypeError, () => Object.getOwnPropertyDescriptor(Object.getPrototypeOf(lock), "locked").get.call({}));

// The lock is not recursive: re-entrant hold throws.
const recursionError = shouldThrow(Error, () => lock.hold(() => lock.hold(() => {})));
shouldBe(recursionError.message, "Lock is not recursive");
// ... and the outer hold's release still happened.
shouldBeFalse(lock.locked);
shouldBe(lock.hold(() => "reacquired"), "reacquired");

// An exception in the callback releases the lock and propagates.
const boom = new Error("boom");
shouldBe(shouldThrow(() => lock.hold(() => { throw boom; })), boom);
shouldBeFalse(lock.locked);
shouldBe(lock.hold(() => "ok-after-throw"), "ok-after-throw");

// Independent locks do not interfere; holding one does not block another.
const lock2 = new Lock();
lock.hold(() => {
    shouldBe(lock2.hold(() => "nested-other-lock"), "nested-other-lock");
    shouldBeTrue(lock.locked);
    shouldBeFalse(lock2.locked);
});

// hold callback receives no arguments and undefined this (sloppy callback
// gets the global; use strict to observe the raw this).
lock.hold(function () {
    "use strict";
    shouldBe(arguments.length, 0);
    shouldBe(this, undefined);
});

// A Lock is an ordinary shareable object: a thread can use a lock created
// by main, including one delivered via a shared object property.
const channel = { lock: new Lock(), value: 0 };
const t = new Thread(ch => {
    ch.lock.hold(() => { ch.value = 123; });
    return ch.lock instanceof Lock;
}, channel);
shouldBeTrue(t.join());
shouldBe(channel.value, 123);
shouldBeFalse(channel.lock.locked);

// toStringTag.
shouldBe(Object.prototype.toString.call(lock), "[object Lock]");
