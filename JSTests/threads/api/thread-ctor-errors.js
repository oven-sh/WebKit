//@ requireOptions("--useJSThreads=1")
// SPEC-api 4.1 constructor and method error cases, exact messages:
// - new Thread(fn): fn callable else TypeError ("Thread constructor requires
//   a callable argument"); no-new => TypeError.
// - self-join => Error ("Thread cannot join itself").
// - incompatible receivers => TypeError.
// Plus the §4 surface shared by all five globals: DontEnum global props,
// no-new TypeErrors, Symbol.toStringTag, CAE shape (4.1: global ctor, Error
// subclass, name "ConcurrentAccessError").
load("../harness.js", "caller relative");

// ---- Thread constructor ----
shouldThrow(TypeError, () => Thread(() => 0), "calling Thread constructor without new is invalid");
shouldThrow(TypeError, () => new Thread(), "Thread constructor requires a callable argument");
shouldThrow(TypeError, () => new Thread(undefined), "Thread constructor requires a callable argument");
shouldThrow(TypeError, () => new Thread(null), "Thread constructor requires a callable argument");
shouldThrow(TypeError, () => new Thread(1), "Thread constructor requires a callable argument");
shouldThrow(TypeError, () => new Thread("function"), "Thread constructor requires a callable argument");
shouldThrow(TypeError, () => new Thread({}), "Thread constructor requires a callable argument");
// A failed spawn must not leak: spawning still works afterwards.
shouldBe(new Thread(() => "ok").join(), "ok");

// ---- self-join: Error, exact message, fired inside the spawned fn ----
{
    const t = new Thread(() => {
        shouldThrow(Error, () => Thread.current.join(), "Thread cannot join itself");
        return "done";
    });
    shouldBe(t.join(), "done");
}

// ---- incompatible receivers (prototype methods/getters extracted) ----
shouldThrow(TypeError, () => Thread.prototype.join.call({}), "Thread.prototype.join called on incompatible receiver");
shouldThrow(TypeError, () => Thread.prototype.asyncJoin.call({}), "Thread.prototype.asyncJoin called on incompatible receiver");
shouldThrow(TypeError, () => Object.getOwnPropertyDescriptor(Thread.prototype, "id").get.call({}), "Thread.prototype.id called on incompatible receiver");

// ---- the four sibling constructors: no-new TypeErrors ----
shouldThrow(TypeError, () => Lock(), "calling Lock constructor without new is invalid");
shouldThrow(TypeError, () => Condition(), "calling Condition constructor without new is invalid");
shouldThrow(TypeError, () => ThreadLocal(), "calling ThreadLocal constructor without new is invalid");

// ---- argument validation on Lock/Condition (4.2/4.3) ----
const lock = new Lock();
const cond = new Condition();
shouldThrow(TypeError, () => lock.hold(), "Lock.prototype.hold requires a callable argument");
shouldThrow(TypeError, () => lock.hold(1), "Lock.prototype.hold requires a callable argument");
shouldThrow(TypeError, () => lock.asyncHold(1), "Lock.prototype.asyncHold requires a callable argument when one is provided");
shouldThrow(TypeError, () => Lock.prototype.hold.call({}, () => 0), "Lock.prototype.hold called on incompatible receiver");
shouldThrow(TypeError, () => Lock.prototype.asyncHold.call({}), "Lock.prototype.asyncHold called on incompatible receiver");

shouldThrow(TypeError, () => cond.wait(), "Condition.prototype.wait requires a Lock argument");
shouldThrow(TypeError, () => cond.wait({}), "Condition.prototype.wait requires a Lock argument");
shouldThrow(TypeError, () => cond.wait(lock), "Condition.prototype.wait requires the lock to be held by the caller");
shouldThrow(TypeError, () => cond.asyncWait(), "Condition.prototype.asyncWait requires a Lock argument");
shouldThrow(TypeError, () => cond.asyncWait(lock), "Condition.prototype.asyncWait requires the lock to be held");
shouldThrow(TypeError, () => Condition.prototype.wait.call({}, lock), "Condition.prototype.wait called on incompatible receiver");
shouldThrow(TypeError, () => Condition.prototype.notify.call({}), "Condition.prototype.notify called on incompatible receiver");

// A wait() that threw "not held" must leave the lock usable.
shouldBe(lock.hold(() => "still-works"), "still-works");

// ---- ConcurrentAccessError shape (4.1) ----
shouldBe(typeof ConcurrentAccessError, "function");
{
    const cae = new ConcurrentAccessError("msg");
    shouldBeTrue(cae instanceof ConcurrentAccessError);
    shouldBeTrue(cae instanceof Error, "CAE must be an Error subclass");
    shouldBe(cae.name, "ConcurrentAccessError");
    shouldBe(cae.message, "msg");
}

// ---- Symbol.toStringTag on every prototype (§4 preamble) ----
shouldBe(Thread.prototype[Symbol.toStringTag], "Thread");
shouldBe(Lock.prototype[Symbol.toStringTag], "Lock");
shouldBe(Condition.prototype[Symbol.toStringTag], "Condition");
shouldBe(ThreadLocal.prototype[Symbol.toStringTag], "ThreadLocal");

// ---- constructors are DontEnum global own props (4 preamble / 9.2-2) ----
for (const name of ["Thread", "Lock", "Condition", "ThreadLocal", "ConcurrentAccessError"]) {
    const desc = Object.getOwnPropertyDescriptor(globalThis, name);
    shouldBeTrue(!!desc, name + " must be an own global property under --useJSThreads=1");
    shouldBeFalse(desc.enumerable, name + " must be DontEnum");
}
