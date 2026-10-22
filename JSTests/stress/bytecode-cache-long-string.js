//@ skip if $memoryLimited and ENV["JSCTEST_memoryLimit"].to_i < 2 * 1024 ** 3
//@ skip if $addressBits <= 32
//@ slow!
//@ runDefault

// A string record of a bytecode cache payload holds a string of any length. Its length field was 27 bits once, and a
// source with one string of 2^27 characters ended the process when it was encoded. The length is what is under test:
// the test needs about 1.3 GB and a second or two, so it is slow! and runs only where the shell may use 2 GB.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + String(actual).slice(0, 64) + ", expected " + String(expected).slice(0, 64));
}

const length = 2 ** 27;
const long = "a".repeat(length - 1) + "b";

// As a string constant: CachedJSValue writes the record and decodePlainString() reads it.
{
    const results = evalTwiceFromTransientBytecodeCache(`var s = "${long}"; s`);
    shouldBe(results.length, 2);
    for (const result of results) {
        shouldBe(result.length, length);
        shouldBe(result, long);
    }
}
gc();

// As a property name: CachedPtr writes the record and decode() makes the atom from it with the stored hash. The lookup
// is by an equal string that is made while the program runs, so it finds the property only under the hash of the
// characters.
{
    const results = evalTwiceFromTransientBytecodeCache(`
        var o = { "${long}": 1, last: 2 };
        var key = "a".repeat(${length - 1}) + "b";
        o[key] + ":" + Object.keys(o).length
    `);
    shouldBe(results.length, 2);
    shouldBe(results[0], "1:2");
    shouldBe(results[1], "1:2");
}
