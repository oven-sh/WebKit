// Without ahead-of-time compiled code (and without --keepDenseArraysDense=1) Object.freeze treats an array as upstream does.
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

function messageOf(operation) {
    try {
        operation();
    } catch (error) {
        shouldBe(error instanceof TypeError, true);
        return error.message;
    }
    return "did not throw";
}

let sum = 0;
Object.defineProperty(Array.prototype, "throughSetter", { set(value) { sum += value; }, configurable: true });

const factories = [
    () => [1, 2, 3],
    () => [1.5, 2.5],
    () => ["a", { }],
    () => {
        const array = [];
        array.push(1);
        return array;
    },
];

for (const create of factories) {
    const array = Object.freeze(create());
    shouldBe(Object.isFrozen(array), true);
    shouldBe($vm.indexingMode(array).startsWith("CopyOnWrite"), false);
    shouldBe(messageOf(() => { "use strict"; array.extra = 5; }), "Attempting to define property on object that is not extensible.");
    shouldBe(messageOf(() => Object.setPrototypeOf(array, null)), "Attempted to assign to readonly property.");
    shouldBe(messageOf(() => { array.__proto__ = null; }) === "did not throw", false);
    array.throughSetter = 1;
}
shouldBe(sum, factories.length);
