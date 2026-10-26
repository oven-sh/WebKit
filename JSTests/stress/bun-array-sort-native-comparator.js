function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldBeArray(actual, expected) {
    shouldBe(actual.length, expected.length);
    for (let i = 0; i < expected.length; ++i)
        shouldBe(actual[i], expected[i]);
}

function shouldThrowTypeError(run) {
    let error = null;
    try {
        run();
    } catch (thrown) {
        error = thrown;
    }
    if (!(error instanceof TypeError))
        throw new Error(`expected a TypeError but got ${error}`);
}

function subtract(a, b) {
    return a - b;
}

const elementCount = 101;

function createShuffledArray() {
    const array = [];
    for (let i = 0; i < elementCount; ++i)
        array.push((i * 41) % elementCount);
    return array;
}

const ascending = [];
for (let i = 0; i < elementCount; ++i)
    ascending.push(i);

function testOrderingComparator(comparator) {
    const array = createShuffledArray();
    shouldBe(array.sort(comparator), array);
    shouldBeArray(array, ascending);

    const source = createShuffledArray();
    const sorted = source.toSorted(comparator);
    shouldBe(sorted === source, false);
    shouldBeArray(sorted, ascending);
    shouldBeArray(source, createShuffledArray());
}

function testBuiltinComparators() {
    const equalUnderComparator = [0.3, 0.1, 0.2];
    shouldBe(equalUnderComparator.sort(Math.trunc), equalUnderComparator);
    shouldBeArray(equalUnderComparator, [0.3, 0.1, 0.2]);

    const array = [3, 1, 2];
    shouldBe(array.sort(Math.min), array);
    shouldBeArray(array.toSorted(), [1, 2, 3]);
}

function testThrowingComparator(comparator) {
    const array = [2, 1, 3];
    shouldThrowTypeError(() => array.sort(comparator));
    shouldBeArray(array, [2, 1, 3]);
    shouldThrowTypeError(() => array.toSorted(comparator));
    shouldBeArray(array, [2, 1, 3]);
}

const { proxy: revokedProxy, revoke } = Proxy.revocable(subtract, {});
revoke();

for (let i = 0; i < 100; ++i) {
    testOrderingComparator(subtract);
    testOrderingComparator(subtract.bind(null));
    testOrderingComparator(new Proxy(subtract, {}));
    testBuiltinComparators();
    testThrowingComparator(Symbol.prototype.valueOf);
    testThrowingComparator(revokedProxy);
}
