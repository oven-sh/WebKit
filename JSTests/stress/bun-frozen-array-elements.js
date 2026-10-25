//@ requireOptions("--useDollarVM=1", "--keepDenseArraysDense=1")
// Object.freeze freezes an ordinary array in place only in a VM that runs ahead-of-time compiled code; the option stands in for such code.
// bun-frozen-array-without-compiled-code.js is the other half: without either, an array is frozen as upstream freezes it.
const loopCount = typeof testLoopCount === "number" ? testLoopCount : 20000;
// False under --alwaysHaveABadTime=true (lockdown mode): every array has slow-put array storage and is frozen generically.
const canFreezeInPlace = !$vm.isHavingABadTime();

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrowTypeError(operation) {
    let error;
    try {
        operation();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error(`expected a TypeError but got ${String(error)}: ${operation}`);
}

function isInPlace(array) {
    return $vm.indexingMode(array).startsWith("CopyOnWrite");
}

function checkElements(array, expectedElements) {
    shouldBe(array.length, expectedElements.length);
    for (let i = 0; i < expectedElements.length; ++i) {
        shouldBe(array[i], expectedElements[i]);
        shouldBe(i in array, i in expectedElements);
    }
}

function check(array, expectedElements) {
    shouldBe(Object.isFrozen(array), true);
    checkElements(array, expectedElements);
    if (!(0 in expectedElements))
        return;
    const descriptor = Object.getOwnPropertyDescriptor(array, 0);
    shouldBe(descriptor.value, expectedElements[0]);
    shouldBe(descriptor.writable, false);
    shouldBe(descriptor.enumerable, true);
    shouldBe(descriptor.configurable, false);
}

function checkInheritor(object, expectedElements) {
    shouldBe(object[0], expectedElements[0]);
    object[0] = 9;
    shouldBe(Object.hasOwn(object, 0), false);
    shouldBe(object[0], expectedElements[0]);
    shouldThrowTypeError(() => {
        "use strict";
        object[0] = 9;
    });
    shouldBe(Object.hasOwn(object, 0), false);
}

// One allocation site per array: optimized code that expects a wider shape converts the array it is handed, and the site that allocated it allocates that shape from then on.
function createFactory(expectedMode, source) {
    return () => {
        const array = new Function(source)();
        if (canFreezeInPlace)
            shouldBe($vm.indexingMode(array), expectedMode);
        return array;
    };
}

const pushFactories = [
    createFactory("ArrayWithInt32", "const array = []; array.push(3); array.push(1); array.push(2); return array;"),
    createFactory("ArrayWithDouble", "const array = []; array.push(2.5); array.push(1.5); array.push(3.5); return array;"),
    createFactory("ArrayWithContiguous", 'const array = []; array.push("c"); array.push("a"); array.push("b"); return array;'),
];

const inPlaceFactories = [
    ...pushFactories,
    createFactory("CopyOnWriteArrayWithInt32", "return [3, 1, 2];"),
    createFactory("CopyOnWriteArrayWithDouble", "return [2.5, 1.5, 3.5];"),
    createFactory("CopyOnWriteArrayWithContiguous", 'return ["c", "a", "b"];'),
];

function forEachFrozenArray(test) {
    for (const create of inPlaceFactories) {
        const array = create();
        const original = array.slice();
        Object.freeze(array);
        shouldBe(isInPlace(array), canFreezeInPlace);
        test(array, original);
    }
}

function testInPlace() {
    const factories = [
        ...inPlaceFactories,
        () => [1, 2, 3],
        () => [1.5, 2.5],
        () => ["a", {}],
    ];
    for (const create of factories) {
        const array = create();
        const original = array.slice();
        shouldBe(Object.freeze(array), array);
        shouldBe(isInPlace(array), canFreezeInPlace);
        check(array, original);

        shouldBe(Object.freeze(array), array);
        shouldBe(isInPlace(array), canFreezeInPlace);
        check(array, original);
    }
}

function testNotInPlace() {
    class DerivedArray extends Array { }
    const factories = [
        () => [],
        () => [1, , 3],
        () => [1.5, , 2.5],
        () => new Array(5),
        () => {
            const array = [1, 2, 3];
            array.x = 1;
            return array;
        },
        () => new DerivedArray(1, 2, 3),
        () => {
            const array = [1, 2, 3];
            array[100000] = 1;
            return array;
        },
    ];
    for (const create of factories) {
        const array = create();
        const original = Array.prototype.slice.call(array);
        Object.freeze(array);
        shouldBe(isInPlace(array), false);
        check(array, original);
    }

    // Freezing the elements of an object that is already a prototype gives its realm a bad time.
    const prototypeArray = createGlobalObject().eval(`
        const array = [1, 2, 3];
        Object.create(array);
        Object.freeze(array);
    `);
    shouldBe(isInPlace(prototypeArray), false);
    check(prototypeArray, [1, 2, 3]);

    const otherRealmArray = createGlobalObject().Array(1, 2);
    Object.freeze(otherRealmArray);
    check(otherRealmArray, [1, 2]);
}

const rejectedStores = [
    "a[0] = 9",
    "a[3] = 9",
    "a.length = 0",
    "a.length = 5",
    "a.x = 1",
];

const rejectedOperations = [
    "Object.defineProperty(a, 0, { value: 9 })",
    'Object.defineProperty(a, "length", { value: 1 })',
    "Object.setPrototypeOf(a, null)",
    "a.push(4)",
    "a.push()",
    "a.pop()",
    "a.shift()",
    "a.unshift(0)",
    "a.splice(0, 1)",
    "a.sort()",
    "a.reverse()",
    "a.fill(0)",
    "a.copyWithin(0, 1)",
];

function testRejectedInMode(prologue) {
    const isStrict = prologue !== "";
    const createOperation = source => new Function("a", `${prologue}return ${source};`);

    for (const source of rejectedStores) {
        const store = createOperation(source);
        forEachFrozenArray((array, original) => {
            if (isStrict)
                shouldThrowTypeError(() => store(array));
            else
                store(array);
            check(array, original);
        });
        for (const create of inPlaceFactories)
            store(create());
    }

    const deleteElement = createOperation("delete a[0]");
    forEachFrozenArray((array, original) => {
        if (isStrict)
            shouldThrowTypeError(() => deleteElement(array));
        else
            shouldBe(deleteElement(array), false);
        check(array, original);
    });
    for (const create of inPlaceFactories)
        shouldBe(deleteElement(create()), true);

    for (const source of rejectedOperations) {
        const operation = createOperation(source);
        forEachFrozenArray((array, original) => {
            shouldThrowTypeError(() => operation(array));
            check(array, original);
        });
        for (const create of inPlaceFactories)
            operation(create());
    }
}

function testRejected() {
    testRejectedInMode('"use strict"; ');
    testRejectedInMode("");

    const empty = Object.freeze([]);
    shouldThrowTypeError(() => empty.pop());
    check(empty, []);
}

function testAllowed() {
    forEachFrozenArray((array, original) => {
        Object.defineProperty(array, 0, { value: original[0] });
        check(array, original);
    });

    forEachFrozenArray((array, original) => {
        shouldBe(delete array[7], true);
        check(array, original);
    });

    const createCopies = array => [
        array.slice(),
        array.concat(),
        array.map(element => element),
        [...array],
        Array.from(array),
    ];
    forEachFrozenArray((array, original) => {
        for (const copy of createCopies(array)) {
            checkElements(copy, original);
            shouldBe(Object.isFrozen(copy), false);
            copy[0] = 9;
            shouldBe(copy[0], 9);
            copy.push(10);
            shouldBe(copy.length, original.length + 1);
        }

        shouldBe(JSON.stringify(array), JSON.stringify(original));
        const iterated = [];
        for (const element of array)
            iterated.push(element);
        checkElements(iterated, original);
        shouldBe(array.at(-1), original[original.length - 1]);
        shouldBe(array.indexOf(original[1]), 1);
        shouldBe(array.includes(original[2]), true);
        shouldBe(array.includes(9), false);
        shouldBe(array.join(), original.join());
        shouldBe(isInPlace(array), canFreezeInPlace);
        check(array, original);
    });
}

// Compiled code holds the kind of an array's storage for proven across these, so none of them may move the elements: a store, a deletion or a new length
// is refused where the array stands, and an array that becomes a prototype stays as it is.
function testStaysInPlace() {
    const refusedOperations = [
        array => { array[0] = 9; },
        array => { array[7] = 9; },
        array => { delete array[0]; },
        array => { array.length = 0; },
        array => shouldThrowTypeError(() => array.push(9)),
    ];
    for (const operation of refusedOperations) {
        forEachFrozenArray((array, original) => {
            operation(array);
            shouldBe(isInPlace(array), canFreezeInPlace);
            check(array, original);
        });
    }

    const prototypeTriggers = [
        array => Object.create(array),
        array => Object.setPrototypeOf({}, array),
    ];
    for (const trigger of prototypeTriggers) {
        forEachFrozenArray((array, original) => {
            const object = trigger(array);
            shouldBe(isInPlace(array), canFreezeInPlace);
            check(array, original);
            checkInheritor(object, original);
            check(array, original);
        });
    }
}

// What has to ask the prototypes, or goes by the ordinary definition of a property, first gives the array the ordinary frozen form: from there on the
// answers and the messages are upstream's own.
function testMoveToSparseMap() {
    const sparseMapTriggers = [
        array => { array.x = 1; },
        array => shouldThrowTypeError(() => array.pop()),
        array => shouldThrowTypeError(() => Object.defineProperty(array, 0, { value: 9 })),
        array => shouldThrowTypeError(() => Object.setPrototypeOf(array, null)),
    ];
    for (const trigger of sparseMapTriggers) {
        forEachFrozenArray((array, original) => {
            trigger(array);
            shouldBe(isInPlace(array), false);
            check(array, original);
        });
    }
}

function testImmutableProperties() {
    forEachFrozenArray((array, original) => {
        shouldBe($vm.makePropertiesImmutable(array), array);
        check(array, original);
        checkInheritor(Object.create(array), original);
        check(array, original);
    });
}

function read(array, index) {
    return array[index];
}

function store(array, index, value) {
    "use strict";
    array[index] = value;
}

function append(array, value) {
    array.push(value);
}

function testTiers() {
    for (const create of pushFactories) {
        const expected = create();
        for (let i = 0; i < loopCount; ++i) {
            const array = create();
            shouldBe(read(array, i % 3), expected[i % 3]);
            store(array, 0, 9);
            shouldBe(array[0], 9);
            append(array, 9);
            shouldBe(array.length, 4);
        }
    }

    for (const create of pushFactories) {
        const expected = create();
        const frozen = Object.freeze(create());
        for (let i = 0; i < loopCount; ++i)
            shouldBe(read(frozen, i % 3), expected[i % 3]);
        shouldBe(isInPlace(frozen), canFreezeInPlace);

        for (const operation of [array => store(array, 0, 9), array => append(array, 9)]) {
            const rejected = Object.freeze(create());
            for (let i = 0; i < loopCount; ++i) {
                shouldThrowTypeError(() => operation(rejected));
                checkElements(rejected, expected);
            }
            check(rejected, expected);

            for (let i = 0; i < loopCount / 10; ++i) {
                const array = Object.freeze(create());
                shouldThrowTypeError(() => operation(array));
                checkElements(array, expected);
            }
        }
    }
}

function testBadTime() {
    const other = createGlobalObject();
    const arrays = other.eval(`[
        Object.freeze([1, 2, 3]),
        Object.freeze([1.5, 2.5]),
        Object.freeze(["a", "b"]),
    ]`);
    const expected = [[1, 2, 3], [1.5, 2.5], ["a", "b"]];
    for (let i = 0; i < arrays.length; ++i) {
        shouldBe(isInPlace(arrays[i]), canFreezeInPlace);
        check(arrays[i], expected[i]);
    }

    other.eval(`Object.defineProperty(Array.prototype, 0, { get() { return 1; }, configurable: true })`);

    for (let i = 0; i < arrays.length; ++i)
        check(arrays[i], expected[i]);
}

for (const test of [testInPlace, testNotInPlace, testRejected, testAllowed, testStaysInPlace, testMoveToSparseMap, testImmutableProperties, testTiers]) {
    test();
    shouldBe($vm.isHavingABadTime(), !canFreezeInPlace);
}
testBadTime();
