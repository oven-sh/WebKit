//@ requireOptions("--useJSThreads=1")
// Thread construction basics: constructor shape, argument validation,
// argument passing, and prototype surface.
load("../resources/assert.js", "caller relative");

shouldBe(typeof Thread, "function");
shouldBe(Thread.length, 1);
shouldBe(Thread.name, "Thread");

// Calling without `new` is invalid.
shouldThrow(TypeError, () => Thread(() => {}));
shouldThrow(TypeError, () => Thread.call(null, () => {}));

// The first argument must be callable.
shouldThrow(TypeError, () => new Thread());
shouldThrow(TypeError, () => new Thread(undefined));
shouldThrow(TypeError, () => new Thread(null));
shouldThrow(TypeError, () => new Thread(42));
shouldThrow(TypeError, () => new Thread("function() {}"));
shouldThrow(TypeError, () => new Thread({}));
shouldThrow(TypeError, () => new Thread([]));
shouldThrow(TypeError, () => new Thread(Symbol()));

// Prototype surface.
shouldBe(typeof Thread.prototype.join, "function");
shouldBe(typeof Thread.prototype.asyncJoin, "function");
shouldBe(typeof Thread.restrict, "function");
shouldBe(Thread.prototype.constructor, Thread);

// join/asyncJoin demand a real Thread receiver.
shouldThrow(TypeError, () => Thread.prototype.join.call({}));
shouldThrow(TypeError, () => Thread.prototype.join.call(null));
shouldThrow(TypeError, () => Thread.prototype.asyncJoin.call({}));
shouldThrow(TypeError, () => Thread.prototype.asyncJoin.call(42));

// Instances.
const t = new Thread(() => {});
shouldBeTrue(t instanceof Thread);
shouldBe(Object.getPrototypeOf(t), Thread.prototype);
t.join();

// Different kinds of callables work.
shouldBe(new Thread(function() { return "plain"; }).join(), "plain");
shouldBe(new Thread(() => "arrow").join(), "arrow");
shouldBe(new Thread(Math.max, 1, 7, 3).join(), 7); // native function
shouldBe(new Thread(function() { "use strict"; return this; }.bind("bound")).join(), "bound"); // bound fn keeps its this
function* gen() { yield 1; }
shouldBe(typeof new Thread(gen).join(), "object"); // generator fn returns a generator

// A class constructor is callable per getCallData, but invoking it without
// `new` throws inside the thread; the TypeError crosses join().
shouldThrow(TypeError, () => new Thread(class Foo {}).join());

// Arguments are forwarded positionally; extra args beyond the function's
// arity are still visible via `arguments`.
const obj = { tag: "shared" };
const got = new Thread(function(a, b, c) {
    return [a, b, c, arguments.length];
}, 1, "two", obj, "extra").join();
shouldBe(got[0], 1);
shouldBe(got[1], "two");
shouldBe(got[2], obj);
shouldBe(got[3], 4);

// No arguments means the function sees none.
shouldBe(new Thread(function() { return arguments.length; }).join(), 0);

// `this` inside the thread body is undefined for strict / globalThis-ish for
// sloppy; we only require that the call succeeds and is consistent.
shouldBe(new Thread(function() { "use strict"; return this; }).join(), undefined);

// Spawning many threads works; each runs its body exactly once.
const counters = { spawned: 0 };
const threads = spawnN(8, (i) => {
    counters.spawned++;
    return i * 2;
});
const results = joinAll(threads);
for (let i = 0; i < 8; ++i)
    shouldBe(results[i], i * 2);
shouldBe(counters.spawned, 8);
