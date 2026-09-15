//@ memoryHog!
//@ slow!
//@ skip if $buildType == "debug"
//@ skip if $addressBits <= 32
//@ runDefault

// Intl.ListFormat collects the strings of its iterable in a Vector<String> before it formats them.
// A Vector holds at most 2^31 bytes, which is 2^28 - 1 strings. format() and formatToParts() have to
// throw when that Vector cannot grow, and close the iterator. They used to crash.
// Each call of test() steps an iterator more than 100 million times, so it takes tens of seconds.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function test(format) {
    let closed = false;
    // This never ends on its own: the only way out is the throw.
    const items = {
        [Symbol.iterator]() {
            return {
                next() { return { done: false, value: "a" }; },
                return() { closed = true; return { }; },
            };
        }
    };

    let error = null;
    try {
        format(items);
    } catch (e) {
        error = e;
    }
    if (String(error) !== "RangeError: Out of memory")
        throw new Error(`bad error: ${String(error)}`);
    shouldBe(closed, true);
}

const listFormat = new Intl.ListFormat("en");
test(items => listFormat.format(items));
test(items => listFormat.formatToParts(items));

shouldBe(listFormat.format(["a", "b", "c"]), "a, b, and c");
