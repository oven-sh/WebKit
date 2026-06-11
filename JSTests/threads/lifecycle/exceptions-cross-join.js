//@ requireOptions("--useJSThreads=1")
// Exceptions thrown inside a thread body cross join() with identity intact,
// rethrow consistently on every join, and arbitrary throwable values
// (not just Errors) are supported.
load("../resources/assert.js", "caller relative");

// Error object identity is preserved across the join boundary.
const boom = new Error("boom");
const failing = new Thread(() => { throw boom; });
shouldBe(shouldThrow(() => failing.join()), boom);
// Rethrows identically on every subsequent join.
shouldBe(shouldThrow(() => failing.join()), boom);
shouldBe(shouldThrow(() => failing.join()), boom);

// Error subclasses keep their type and message.
class CustomError extends Error {
    constructor(message) {
        super(message);
        this.name = "CustomError";
    }
}
const custom = shouldThrow(CustomError, () => new Thread(() => { throw new CustomError("custom"); }).join());
shouldBe(custom.message, "custom");
shouldBeTrue(custom instanceof Error);

// Builtin error types raised organically inside the thread cross join.
shouldThrow(TypeError, () => new Thread(() => null.f).join());
shouldThrow(ReferenceError, () => new Thread(() => definitelyNotDefined).join());
shouldThrow(RangeError, () => new Thread(() => new Array(-1)).join());
// eval of a constant malformed literal is intentional here: it is the
// simplest way to raise a genuine SyntaxError inside the thread body.
shouldThrow(SyntaxError, () => new Thread(() => eval("{")).join());

// Non-Error throwable values come through by value/identity.
shouldBe(shouldThrow(() => new Thread(() => { throw "a string"; }).join()), "a string");
shouldBe(shouldThrow(() => new Thread(() => { throw 42; }).join()), 42);
shouldBe(shouldThrow(() => new Thread(() => { throw null; }).join()), null);
shouldBe(shouldThrow(() => new Thread(() => { throw undefined; }).join()), undefined);
shouldBe(shouldThrow(() => new Thread(() => { throw NaN; }).join()), NaN);
const thrownObj = { reason: "object" };
shouldBe(shouldThrow(() => new Thread(o => { throw o; }, thrownObj).join()), thrownObj);
const thrownSym = Symbol("thrown");
shouldBe(shouldThrow(() => new Thread(s => { throw s; }, thrownSym).join()), thrownSym);

// The thrown error is a live shared object: mutations made by the catcher
// are visible everywhere.
const mutable = new Error("original");
const t = new Thread(() => { throw mutable; });
const caught = shouldThrow(() => t.join());
caught.extra = "annotated";
shouldBe(mutable.extra, "annotated");
shouldBe(shouldThrow(() => t.join()).extra, "annotated");

// An exception thrown after side effects: the side effects still happened.
const log = [];
const partial = new Thread(l => {
    l.push("before");
    throw new Error("mid");
}, log);
shouldThrow(Error, () => partial.join());
shouldBe(log.length, 1);
shouldBe(log[0], "before");

// A thread can catch its own exceptions and return normally.
shouldBe(new Thread(() => {
    try {
        throw new Error("contained");
    } catch (e) {
        return "caught:" + e.message;
    }
}).join(), "caught:contained");

// A thread can catch a child thread's exception; the parent's join then
// succeeds with the captured error as a value.
const childError = new Error("child failed");
const parent = new Thread(err => {
    const child = new Thread(e => { throw e; }, err);
    try {
        child.join();
        return "child did not throw";
    } catch (caughtError) {
        return caughtError;
    }
}, childError);
shouldBe(parent.join(), childError);

// Stack overflow inside a thread surfaces as a catchable RangeError at join.
shouldThrow(RangeError, () => new Thread(function f() { return f(); }).join());

// finally blocks run inside the thread before the exception escapes.
const order = [];
shouldThrow(Error, () => new Thread(o => {
    try {
        throw new Error("escapes");
    } finally {
        o.push("finally-ran");
    }
}, order).join());
shouldBe(order[0], "finally-ran");
