//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// ConcurrentAccessError is an Error subclass in the NativeError shape: the
// constructor inherits from Error, engine-thrown and constructed instances
// share the realm's one cached Structure, the constructor honors newTarget,
// engine-thrown instances carry no own `cause`, and the engine's throws do
// not depend on the (writable) global binding.
load("../harness.js", "caller relative");

shouldBe(Object.getPrototypeOf(ConcurrentAccessError), Error, "constructor inherits from Error");
shouldBe(Object.getPrototypeOf(ConcurrentAccessError.prototype), Error.prototype);
shouldBe(ConcurrentAccessError.prototype.name, "ConcurrentAccessError");

// ---- engine-thrown instance ----
const restricted = Thread.restrict({ x: 1 });
let thrown;
new Thread(() => {
    try {
        restricted.x;
    } catch (e) {
        thrown = e;
    }
}).join();
shouldBeTrue(thrown instanceof ConcurrentAccessError, "foreign read throws a ConcurrentAccessError");
shouldBe(thrown.name, "ConcurrentAccessError");
shouldBe(thrown.constructor, ConcurrentAccessError);
shouldBeFalse(Object.hasOwn(thrown, "cause"), "engine-thrown instance has no own cause");
shouldBeFalse("cause" in thrown);

// ---- constructed and called instances ----
const constructed = new ConcurrentAccessError("m");
shouldBe(constructed.message, "m");
shouldBeFalse(Object.hasOwn(constructed, "cause"));
shouldBe(new ConcurrentAccessError("m", { cause: 42 }).cause, 42);
const called = ConcurrentAccessError("c");
shouldBeTrue(called instanceof ConcurrentAccessError);
shouldBe(called.message, "c");
// Like every NativeError, the native constructor frame is not part of the stack.
shouldBeFalse(/^ConcurrentAccessError@/.test(String(constructed.stack)), "stack starts at the caller");

// ---- one Structure per realm (same root Structure for every instance) ----
const rootStructureOf = o => $vm.getStructureTransitionList(o)[0];
shouldBe(rootStructureOf(new ConcurrentAccessError("a")), rootStructureOf(new ConcurrentAccessError("b")), "constructed instances share a Structure");
shouldBe(rootStructureOf(constructed), rootStructureOf(thrown), "engine-thrown instances share it too");
shouldBe(rootStructureOf(constructed), rootStructureOf(called));

// ---- the engine does not read the global binding ----
const savedConstructor = ConcurrentAccessError;
globalThis.ConcurrentAccessError = function Fake() {};
let thrownAfterRebind;
new Thread(() => {
    try {
        restricted.x;
    } catch (e) {
        thrownAfterRebind = e;
    }
}).join();
globalThis.ConcurrentAccessError = savedConstructor;
shouldBeTrue(thrownAfterRebind instanceof ConcurrentAccessError, "engine-thrown instance ignores the rebound global");

// ---- subclassing honors newTarget ----
class MyError extends ConcurrentAccessError {
    constructor(message) {
        super(message);
        this.extra = true;
    }
}
const mine = new MyError("sub");
shouldBeTrue(mine instanceof MyError, "subclass instance is an instanceof the subclass");
shouldBeTrue(mine instanceof ConcurrentAccessError);
shouldBeTrue(mine instanceof Error);
shouldBe(Object.getPrototypeOf(mine), MyError.prototype);
shouldBe(mine.message, "sub");
shouldBeTrue(mine.extra);

function Other() {}
shouldBe(Object.getPrototypeOf(Reflect.construct(ConcurrentAccessError, ["r"], Other)), Other.prototype, "Reflect.construct uses newTarget.prototype");
function NoProto() {}
NoProto.prototype = null;
shouldBe(Object.getPrototypeOf(Reflect.construct(ConcurrentAccessError, [], NoProto)), ConcurrentAccessError.prototype,
    "a non-object newTarget.prototype falls back to ConcurrentAccessError.prototype");
