//@ requireOptions("--useJSThreads=1")
// SPEC-api 4.5 error cases for the property path (exact messages), the
// dispatch steps 2-3 (ToPropertyKey on arg1; non-object non-view arg0 =>
// TypeError as today), and the view-vs-object discriminator (a Float64Array
// has own indexed properties, so taking the wrong dispatch branch is
// observable).
load("../harness.js", "caller relative");

const proto = { inherited: 1 };
const o = Object.create(proto);
o.own = 1;
Object.defineProperty(o, "acc", { get() { return 2; }, set() {}, configurable: true });
Object.defineProperty(o, "ro", { value: 3, writable: false, configurable: true });

// ---- load: absent / accessor / proto-chain-only => TypeError ----
shouldThrow(TypeError, () => Atomics.load(o, "absent"), "Atomics.load: object has no own property");
shouldThrow(TypeError, () => Atomics.load(o, "acc"), "Atomics.load: object has no own property");
shouldThrow(TypeError, () => Atomics.load(o, "inherited"), "Atomics.load: object has no own property");
shouldBe(Atomics.load(o, "own"), 1);
shouldBe(Atomics.load(o, "ro"), 3, "non-writable own data is loadable");

// ---- store: accessor / non-writable / absent on non-extensible ----
shouldThrow(TypeError, () => Atomics.store(o, "acc", 1), "Atomics.store: property is an accessor");
shouldThrow(TypeError, () => Atomics.store(o, "ro", 1), "Atomics.store: property is not writable");
shouldBe(o.ro, 3, "failed store must not write");
{
    const sealed = Object.preventExtensions({ has: 1 });
    shouldThrow(TypeError, () => Atomics.store(sealed, "nope", 1),
        "Atomics.store: cannot add a property to a non-extensible object");
    shouldBe(Atomics.store(sealed, "has", 2), 2, "existing own data on a non-extensible object is storable");
    shouldBe(sealed.has, 2);
}
{
    const frozen = Object.freeze({ f: 1 });
    shouldThrow(TypeError, () => Atomics.store(frozen, "f", 2), "Atomics.store: property is not writable");
    shouldBe(frozen.f, 1);
}

// ---- exchange / compareExchange: require existing own DATA property ----
shouldThrow(TypeError, () => Atomics.exchange(o, "absent", 1), "Atomics.exchange: object has no own data property");
shouldThrow(TypeError, () => Atomics.exchange(o, "inherited", 1), "Atomics.exchange: object has no own data property");
shouldThrow(TypeError, () => Atomics.exchange(o, "ro", 1), "Atomics.exchange: property is not writable");
shouldThrow(TypeError, () => Atomics.compareExchange(o, "absent", 1, 2), "Atomics.compareExchange: object has no own data property");
shouldThrow(TypeError, () => Atomics.compareExchange(o, "acc", 1, 2), "Atomics.compareExchange: object has no own data property");
// CAS inherits store's writability rule, thrown unconditionally — both with
// a matching expected value (the write that would corrupt the ReadOnly slot)
// and a non-matching one (no silent value-read fallback).
shouldThrow(TypeError, () => Atomics.compareExchange(o, "ro", 3, 9), "Atomics.compareExchange: property is not writable");
shouldThrow(TypeError, () => Atomics.compareExchange(o, "ro", 999, 9), "Atomics.compareExchange: property is not writable");
shouldBe(o.ro, 3, "rejected CAS must not write");
{
    // The advertised lock-building case: a lock word on an object someone
    // later Object.freeze()s must FAIL to CAS/RMW, never keep mutating.
    const frozen = Object.freeze({ word: 0 });
    shouldThrow(TypeError, () => Atomics.compareExchange(frozen, "word", 0, 1), "Atomics.compareExchange: property is not writable");
    shouldThrow(TypeError, () => Atomics.add(frozen, "word", 1), "Atomics RMW: property is not writable");
    shouldThrow(TypeError, () => Atomics.sub(frozen, "word", 1), "Atomics RMW: property is not writable");
    shouldThrow(TypeError, () => Atomics.or(frozen, "word", 1), "Atomics RMW: property is not writable");
    shouldThrow(TypeError, () => Atomics.exchange(frozen, "word", 1), "Atomics.exchange: property is not writable");
    shouldBe(frozen.word, 0, "frozen lock word unchanged");
    // Writability precedes the stored-value type check.
    const frozenStr = Object.freeze({ s: "x" });
    shouldThrow(TypeError, () => Atomics.add(frozenStr, "s", 1), "Atomics RMW: property is not writable");
}

// ---- RMW family: own data + stored number required ----
shouldThrow(TypeError, () => Atomics.add(o, "absent", 1), "Atomics RMW: object has no own data property");
shouldThrow(TypeError, () => Atomics.sub(o, "inherited", 1), "Atomics RMW: object has no own data property");
shouldThrow(TypeError, () => Atomics.and(o, "acc", 1), "Atomics RMW: object has no own data property");
o.str = "x";
shouldThrow(TypeError, () => Atomics.add(o, "str", 1), "Atomics RMW: stored value is not a number");
shouldThrow(TypeError, () => Atomics.xor(o, "str", 1), "Atomics RMW: stored value is not a number");
o.bigSlot = 1n;
shouldThrow(TypeError, () => Atomics.add(o, "bigSlot", 1), "Atomics RMW: stored value is not a number");
// ...but exchange is store-shaped: non-number stored values are fine.
shouldBe(Atomics.exchange(o, "str", 7), "x");
shouldBe(o.str, 7);

// ---- wait/waitAsync validate like load (own data property required) ----
shouldThrow(TypeError, () => Atomics.wait(o, "absent", 0));
shouldThrow(TypeError, () => Atomics.wait(o, "acc", 0));
shouldThrow(TypeError, () => Atomics.waitAsync(o, "absent", 0));
// notify never requires the property (4.5: 0 valid even if o lacks k)
shouldBe(Atomics.notify(o, "absent"), 0);

// ---- dispatch step 3: non-object, non-view arg0 => TypeError (as today) ----
shouldThrow(TypeError, () => Atomics.load(1, 0));
shouldThrow(TypeError, () => Atomics.load("str", 0));
shouldThrow(TypeError, () => Atomics.store(null, 0, 1));
shouldThrow(TypeError, () => Atomics.store(undefined, 0, 1));
shouldThrow(TypeError, () => Atomics.add(true, 0, 1));
shouldThrow(TypeError, () => Atomics.compareExchange(2n, 0, 1, 2));
shouldThrow(TypeError, () => Atomics.wait(false, 0, 0));
shouldThrow(TypeError, () => Atomics.waitAsync(Symbol("s"), 0, 0));
shouldThrow(TypeError, () => Atomics.notify(null, 0));

// ---- step 1: ANY JSArrayBufferView stays on the TA path — a Float64Array
// HAS an own property "0", so reaching the property path would SUCCEED;
// today's TA path rejects float views for load with TypeError ----
shouldThrow(TypeError, () => Atomics.load(new Float64Array(1), 0));
shouldThrow(TypeError, () => Atomics.load(new Float32Array(1), 0));
// DataView is a view too: TA path rejects it (it is not an integer TA),
// it must not fall through to the property path.
shouldThrow(TypeError, () => Atomics.load(new DataView(new ArrayBuffer(8)), "byteLength"));

// ---- step 2: ToPropertyKey runs on arg1 (and its exceptions propagate,
// before the own-property check) ----
{
    const keyBoom = new Error("key-boom");
    shouldThrow(Error, () => Atomics.load(o, { toString() { throw keyBoom; } }), "key-boom");
    let coerced = false;
    shouldThrow(TypeError, () => Atomics.load(o, { toString() { coerced = true; return "absent"; } }));
    shouldBeTrue(coerced, "key coercion precedes the own-property check");
}
// Symbols are valid keys end-to-end.
{
    const sym = Symbol("sk");
    o[sym] = 4;
    shouldBe(Atomics.load(o, sym), 4);
    shouldBe(Atomics.add(o, sym, 1), 4);
    shouldBe(o[sym], 5);
}

// ---- value/operand coercions still run AFTER validation succeeds ----
{
    o.num = 1;
    let effects = "";
    shouldBe(Atomics.add(o, "num", { valueOf() { effects += "v"; return 1; } }), 1);
    shouldBe(effects, "v");
}

// ---- isLockFree/pause unchanged by the flag (4.5 preamble) ----
shouldBe(typeof Atomics.isLockFree(4), "boolean");
if (Atomics.pause)
    shouldBe(Atomics.pause(), undefined);
