//@ requireOptions("--useJSThreads=1")
// Values returned from a thread body come back from join() with full
// fidelity: primitives by value (including edge cases), heap values by
// identity (the heap is shared; nothing is cloned).
load("../resources/assert.js", "caller relative");

function roundTrip(value) {
    return new Thread(v => v, value).join();
}

// Primitives by value.
shouldBe(new Thread(() => 42).join(), 42);
shouldBe(new Thread(() => {}).join(), undefined); // no return statement
shouldBe(new Thread(() => undefined).join(), undefined);
shouldBe(new Thread(() => null).join(), null);
shouldBe(new Thread(() => true).join(), true);
shouldBe(new Thread(() => false).join(), false);
shouldBe(new Thread(() => "string").join(), "string");
shouldBe(new Thread(() => "").join(), "");

// Numeric edge cases (shouldBe distinguishes -0 and NaN).
shouldBe(new Thread(() => NaN).join(), NaN);
shouldBe(new Thread(() => -0).join(), -0);
shouldBe(new Thread(() => 0).join(), 0);
shouldBe(new Thread(() => Infinity).join(), Infinity);
shouldBe(new Thread(() => -Infinity).join(), -Infinity);
shouldBe(new Thread(() => 2 ** 53).join(), 9007199254740992);
shouldBe(new Thread(() => 0.1 + 0.2).join(), 0.30000000000000004);

// BigInt.
shouldBe(new Thread(() => 123n).join(), 123n);
shouldBe(new Thread(() => -(2n ** 100n)).join(), -(2n ** 100n));

// Symbols keep identity across the join boundary.
const sym = Symbol("mine");
shouldBe(roundTrip(sym), sym);
shouldBe(new Thread(() => Symbol.iterator).join(), Symbol.iterator);
const registered = new Thread(() => Symbol.for("threads-test")).join();
shouldBe(registered, Symbol.for("threads-test"));

// Heap values come back by identity, not by copy.
const obj = { f: 1 };
shouldBe(roundTrip(obj), obj);
const arr = [1, 2, 3];
shouldBe(roundTrip(arr), arr);
function fn() {}
shouldBe(roundTrip(fn), fn);

// An object allocated inside the thread is a real shared object afterwards.
const made = new Thread(() => ({ x: 1, nested: { y: 2 } })).join();
shouldBe(made.x, 1);
shouldBe(made.nested.y, 2);
made.x = 10; // mutable from the joining thread
shouldBe(made.x, 10);

// Arrays allocated in the thread behave normally on the main thread.
const list = new Thread(() => {
    const a = [];
    for (let i = 0; i < 100; ++i)
        a.push(i);
    return a;
}).join();
shouldBe(list.length, 100);
shouldBe(list[99], 99);
shouldBe(list.reduce((s, v) => s + v, 0), 4950);

// Functions (closures) created in a thread are callable after join, and
// still see their captured environment.
const counterFn = new Thread(() => {
    let count = 0;
    return () => ++count;
}).join();
shouldBe(counterFn(), 1);
shouldBe(counterFn(), 2);

// Returning the Thread's own argument list element keeps identity.
const key = {};
shouldBe(new Thread((a, b) => (a === key ? b : "wrong"), key, "ok").join(), "ok");

// Returning a Promise returns the promise object itself (join does not
// await it).
const p = Promise.resolve(7);
shouldBe(roundTrip(p), p);
shouldBeTrue(new Thread(() => Promise.resolve(1)).join() instanceof Promise);

// A returned class instance keeps its prototype chain.
class Point { constructor(x, y) { this.x = x; this.y = y; } norm2() { return this.x * this.x + this.y * this.y; } }
const pt = new Thread(() => new Point(3, 4)).join();
shouldBeTrue(pt instanceof Point);
shouldBe(pt.norm2(), 25);
