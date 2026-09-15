//@ memoryHog!
//@ slow!
//@ skip if $buildType == "debug"
//@ skip if $addressBits <= 32
//@ runDefault

// A FinalizationRegistry keeps its registrations in Vectors: one for the registrations without an
// unregister token, and one per token. A Vector holds at most 2^31 bytes, which is 2^27 - 1
// registrations. register() has to throw when its Vector cannot grow. It used to crash.
// Each call of test() registers more than 100 million times, so it takes seconds.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function test(token) {
    const registry = new FinalizationRegistry(() => { });
    const target = { };
    let error = null;
    let count = 0;
    try {
        for (; count < 2 ** 27; ++count)
            registry.register(target, 1, token);
    } catch (e) {
        error = e;
    }
    if (String(error) !== "RangeError: Out of memory")
        throw new Error(`bad error: ${String(error)} after ${count} registrations`);

    // The registry still works after the throw.
    if (token) {
        shouldBe(registry.unregister(token), true);
        shouldBe(registry.unregister(token), false);
        registry.register(target, 1, token);
        shouldBe(registry.unregister(token), true);
    }
}

test(undefined);
fullGC();
test({ });
