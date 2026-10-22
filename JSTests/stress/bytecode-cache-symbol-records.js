//@ runDefault

// A symbol record of a bytecode cache payload says what kind of symbol it is in the word where a string record has the
// ordinal of its string. Each kind has to come back out of the cache as the symbol that went in.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + ", expected " + expected);
}

// The code of a builtin: a well-known symbol and a private name.
{
    const source = `(function (iterable, promise, onFulfilled) {
        "use strict";
        var iterator = iterable.@@iterator();
        promise.@then(onFulfilled);
        return iterator.next().value;
    })`;
    for (const roundTrip of [false, true]) {
        let fulfilled;
        const builtin = builtinFromBytecodeCache(source, roundTrip);
        shouldBe(builtin([7, 8], Promise.resolve(9), value => { fulfilled = value; }), 7);
        drainMicrotasks();
        shouldBe(fulfilled, 9);
    }
}

// The code of a program has them too: @@iterator for the loop, private names for the arrow function's `this`, the field
// and `super`.
{
    const results = evalTwiceFromTransientBytecodeCache(`
        var sum = 0;
        for (var value of [1, 2, 3])
            sum += value;
        var seven = function () { return () => this; }.call(7)();
        class Base { method() { return 4; } }
        class Derived extends Base {
            field = 5;
            method() { return super.method() + this.field; }
        }
        sum + seven + new Derived().method()
    `);
    shouldBe(results.length, 2);
    shouldBe(results[0], 22);
    shouldBe(results[1], 22);
}
