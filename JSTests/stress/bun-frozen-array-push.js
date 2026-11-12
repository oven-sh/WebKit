//@ requireOptions("--useDollarVM=1", "--keepDenseArraysDense=1")
// Object.freeze keeps the elements of an ordinary array in copy-on-write storage in a VM that runs ahead-of-time compiled code; the option stands in for such
// code (see bun-frozen-array-elements.js). push() has to throw for that array and leave its elements alone in every tier, with no arguments too, whatever
// arrays the call site saw before.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected: ${String(expected)}`);
}

function shouldThrowTypeError(func) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof TypeError, true);
}

const makers = [
    () => [1, 2, 3],
    () => [1.5, 2.5, 3.5],
    () => ["a", "b", "c"],
];
const values = [4, 4.5, "d"];

function makeCallSites() {
    return [
        new Function("array", "value", "return array.push()"),
        new Function("array", "value", "return array.push(value)"),
        new Function("array", "value", "return array.push(value, value)"),
    ];
}

function checkFrozen(push, make, value) {
    const array = Object.freeze(make());
    const expected = String(make());
    if (!$vm.isHavingABadTime())
        shouldBe($vm.indexingMode(array).startsWith("CopyOnWrite"), true);
    shouldThrowTypeError(() => push(array, value));
    shouldBe(array.length, 3);
    shouldBe(String(array), expected);
    shouldBe(Object.isFrozen(array), true);
}

for (let kind = 0; kind < makers.length; ++kind) {
    const make = makers[kind];
    const value = values[kind];

    // Only frozen arrays.
    for (const push of makeCallSites()) {
        noInline(push);
        for (let i = 0; i < testLoopCount / 10; ++i)
            checkFrozen(push, make, value);
    }

    // Both from the beginning. The call site has seen a result and arrays that are not of the original structure only, so it checks the indexing type.
    makeCallSites().forEach((push, argumentCount) => {
        noInline(push);
        for (let i = 0; i < testLoopCount; ++i) {
            if (i % 4)
                shouldBe(push(make(), value), 3 + argumentCount);
            else
                checkFrozen(push, make, value);
        }
    });

    // Frozen arrays after the call site was compiled for writable ones.
    makeCallSites().forEach((push, argumentCount) => {
        noInline(push);
        for (let i = 0; i < testLoopCount; ++i) {
            const array = make();
            shouldBe(push(array, value), 3 + argumentCount);
            shouldBe(array.length, 3 + argumentCount);
        }
        for (let i = 0; i < 10; ++i)
            checkFrozen(push, make, value);
    });
}
