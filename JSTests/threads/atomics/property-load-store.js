//@ requireOptions("--useJSThreads=1")
// SPEC-api 4.5: Atomics.load/store on (object, propertyName) — value and
// key semantics, single thread. (Error cases: property-errors.js; SVZ:
// property-cas-samevaluezero.js; RMW family: property-rmw.js; multi-thread
// exactness: API-I15 in property-rmw.js / races/counter-atomics.js.)
load("../harness.js", "caller relative");

const o = { x: 1 };

// load reads the own data property; store returns v and writes it.
shouldBe(Atomics.load(o, "x"), 1);
shouldBe(Atomics.store(o, "x", 2), 2);
shouldBe(o.x, 2);
shouldBe(Atomics.load(o, "x"), 2);

// Unlike the typed-array path, the property path does NOT coerce the value:
// store returns and stores v itself.
shouldBe(Atomics.store(o, "x", 7.9), 7.9);
shouldBe(o.x, 7.9);

// store creates an absent own property on an extensible object, with
// default (writable/enumerable/configurable) attributes.
shouldBe(Atomics.store(o, "fresh", "v"), "v");
shouldBe(o.fresh, "v");
{
    const desc = Object.getOwnPropertyDescriptor(o, "fresh");
    shouldBeTrue(desc.writable && desc.enumerable && desc.configurable);
}

// store on an EXISTING property must preserve its attributes (4.5: ops only
// change the value — no attribute-stripping transition).
{
    const target = {};
    Object.defineProperty(target, "pinned", { value: 1, writable: true, enumerable: false, configurable: false });
    shouldBe(Atomics.store(target, "pinned", 2), 2);
    const desc = Object.getOwnPropertyDescriptor(target, "pinned");
    shouldBe(desc.value, 2);
    shouldBeFalse(desc.enumerable, "store must not flip enumerable");
    shouldBeFalse(desc.configurable, "store must not flip configurable");
    shouldBeTrue(desc.writable);
}

// Any JS value round-trips by identity / SameValue.
const ref = { deep: true };
Atomics.store(o, "obj", ref);
shouldBe(Atomics.load(o, "obj"), ref);
Atomics.store(o, "u", undefined);
shouldBe(Atomics.load(o, "u"), undefined);
shouldBeTrue("u" in o, "an undefined store still creates the property");
Atomics.store(o, "nil", null);
shouldBe(Atomics.load(o, "nil"), null);
{
    Atomics.store(o, "nan", NaN);
    const back = Atomics.load(o, "nan");
    shouldBeTrue(back !== back);
}
Atomics.store(o, "negz", -0);
shouldBe(Atomics.load(o, "negz"), -0);
{
    const symValue = Symbol("v");
    Atomics.store(o, "symv", symValue);
    shouldBe(Atomics.load(o, "symv"), symValue);
}
{
    const big = 123n;
    Atomics.store(o, "big", big);
    shouldBe(Atomics.load(o, "big"), big);
}

// ---- property keys: ToPropertyKey (4.5 step 2) ----
// Symbols are valid keys.
{
    const key = Symbol("key");
    shouldBe(Atomics.store(o, key, 9), 9);
    shouldBe(Atomics.load(o, key), 9);
    shouldBe(o[key], 9);
}
// Numbers coerce to canonical string/index keys.
shouldBe(Atomics.store(o, 1, "one"), "one");
shouldBe(o[1], "one");
shouldBe(Atomics.load(o, "1"), "one");
// Objects coerce via toString.
shouldBe(Atomics.store(o, { toString() { return "coerced"; } }, 5), 5);
shouldBe(o.coerced, 5);

// ---- indexed properties on arrays take the property path too (an Array is
// not a JSArrayBufferView) ----
{
    const arr = [10, 20];
    shouldBe(Atomics.load(arr, 0), 10);
    shouldBe(Atomics.store(arr, 1, 21), 21);
    shouldBe(arr[1], 21);
    // creating one past the end grows the array like a direct indexed put
    shouldBe(Atomics.store(arr, 2, 30), 30);
    shouldBe(arr[2], 30);
    shouldBe(arr.length, 3);
}

// load on an inline (cell) property and an out-of-line property both work.
{
    const wide = {};
    for (let i = 0; i < 64; ++i)
        wide["p" + i] = i;
    shouldBe(Atomics.load(wide, "p0"), 0);    // inline
    shouldBe(Atomics.load(wide, "p63"), 63);  // out-of-line butterfly
    shouldBe(Atomics.store(wide, "p63", -63), -63);
    shouldBe(wide.p63, -63);
}
