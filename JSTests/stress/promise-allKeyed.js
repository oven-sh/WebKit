//@ requireOptions("--usePromiseAllKeyed=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldBeArray(actual, expected) {
    shouldBe(actual.length, expected.length);
    for (let i = 0; i < expected.length; ++i)
        shouldBe(actual[i], expected[i]);
}

function notReached() {
    throw new Error("should not reach here");
}

shouldBe(typeof Promise.allKeyed, "function");
shouldBe(Promise.allKeyed.length, 1);
shouldBe(Promise.allKeyed.name, "allKeyed");
shouldBe(typeof Promise.allSettledKeyed, "function");
shouldBe(Promise.allSettledKeyed.length, 1);
shouldBe(Promise.allSettledKeyed.name, "allSettledKeyed");

let allKeyedDesc = Object.getOwnPropertyDescriptor(Promise, "allKeyed");
shouldBe(allKeyedDesc.writable, true);
shouldBe(allKeyedDesc.enumerable, false);
shouldBe(allKeyedDesc.configurable, true);

async function test() {
    // Basic: resolves to a null-prototype object with the same keys.
    {
        let result = await Promise.allKeyed({ a: 1, b: Promise.resolve(2), c: Promise.resolve(3) });
        shouldBe(Object.getPrototypeOf(result), null);
        shouldBeArray(Object.keys(result), ["a", "b", "c"]);
        shouldBe(result.a, 1);
        shouldBe(result.b, 2);
        shouldBe(result.c, 3);
    }

    // Empty object resolves to an empty null-prototype object.
    {
        let result = await Promise.allKeyed({});
        shouldBe(Object.getPrototypeOf(result), null);
        shouldBe(Reflect.ownKeys(result).length, 0);
    }

    // Non-object argument rejects with TypeError.
    for (let value of [undefined, null, 42, "str", true, Symbol()]) {
        try {
            await Promise.allKeyed(value);
            notReached();
        } catch (e) {
            shouldBe(e instanceof TypeError, true);
        }
    }

    // Rejection propagates for allKeyed.
    {
        let err = new Error("boom");
        try {
            await Promise.allKeyed({ a: Promise.resolve(1), b: Promise.reject(err) });
            notReached();
        } catch (e) {
            shouldBe(e, err);
        }
    }

    // Key order follows own-property-key order, not settlement order.
    {
        let resolveA, resolveB, resolveC;
        let input = {
            a: new Promise(r => { resolveA = r; }),
            b: new Promise(r => { resolveB = r; }),
            c: new Promise(r => { resolveC = r; }),
        };
        let combined = Promise.allKeyed(input);
        resolveC("C");
        resolveA("A");
        resolveB("B");
        let result = await combined;
        shouldBeArray(Object.keys(result), ["a", "b", "c"]);
        shouldBe(result.a, "A");
        shouldBe(result.b, "B");
        shouldBe(result.c, "C");
    }

    // Symbol keys are included; non-enumerable keys are skipped.
    {
        let sym = Symbol("s");
        let hidden = Symbol("hidden");
        let input = { str: Promise.resolve(1) };
        input[sym] = Promise.resolve(2);
        Object.defineProperty(input, "nonenum", { enumerable: false, value: Promise.resolve(3) });
        Object.defineProperty(input, hidden, { enumerable: false, value: Promise.resolve(4) });
        let result = await Promise.allKeyed(input);
        let keys = Reflect.ownKeys(result);
        shouldBe(keys.length, 2);
        shouldBe(keys[0], "str");
        shouldBe(keys[1], sym);
        shouldBe(result.str, 1);
        shouldBe(result[sym], 2);
        shouldBe(Object.prototype.hasOwnProperty.call(result, "nonenum"), false);
        shouldBe(Object.prototype.hasOwnProperty.call(result, hidden), false);
    }

    // Inherited properties are ignored.
    {
        let proto = { inherited: Promise.resolve("nope") };
        let input = Object.create(proto);
        input.own = Promise.resolve("yes");
        let result = await Promise.allKeyed(input);
        shouldBeArray(Object.keys(result), ["own"]);
        shouldBe(result.own, "yes");
        shouldBe(Object.prototype.hasOwnProperty.call(result, "inherited"), false);
    }

    // allSettledKeyed: fulfilled/rejected entries.
    {
        let err = new Error("rej");
        let result = await Promise.allSettledKeyed({
            ok: Promise.resolve(1),
            bad: Promise.reject(err),
            plain: 2,
        });
        shouldBe(Object.getPrototypeOf(result), null);
        shouldBeArray(Object.keys(result), ["ok", "bad", "plain"]);
        shouldBe(result.ok.status, "fulfilled");
        shouldBe(result.ok.value, 1);
        shouldBe(result.bad.status, "rejected");
        shouldBe(result.bad.reason, err);
        shouldBe(result.plain.status, "fulfilled");
        shouldBe(result.plain.value, 2);
    }

    // allSettledKeyed: empty object.
    {
        let result = await Promise.allSettledKeyed({});
        shouldBe(Object.getPrototypeOf(result), null);
        shouldBe(Reflect.ownKeys(result).length, 0);
    }

    // allSettledKeyed: non-object rejects.
    try {
        await Promise.allSettledKeyed(null);
        notReached();
    } catch (e) {
        shouldBe(e instanceof TypeError, true);
    }

    // Non-constructor this throws synchronously (from NewPromiseCapability).
    {
        let threw = false;
        try {
            Promise.allKeyed.call(eval);
        } catch (e) {
            threw = e instanceof TypeError;
        }
        shouldBe(threw, true);
    }

    // C.resolve not callable rejects.
    {
        class Sub extends Promise {}
        Sub.resolve = null;
        try {
            await Sub.allKeyed({ a: 1 });
            notReached();
        } catch (e) {
            shouldBe(e instanceof TypeError, true);
        }
    }
}

test().then(
    () => {},
    e => { print(e.stack || e); $vm.abort(); }
);
drainMicrotasks();
