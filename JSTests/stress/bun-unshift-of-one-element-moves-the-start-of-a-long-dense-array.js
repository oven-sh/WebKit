//@ requireOptions("--useDollarVM=1", "--keepDenseArraysDense=1")
// Bun: where dense arrays stay dense (a compiled program), unshift() of one element onto an array above the shift threshold moves the start of its
// elements and copies nothing. JSArray::fastUnshift() (WebKit/WebKit@293f970fc4), which copies the elements in place, leaves such an array alone.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + ", got " + actual);
}

function butterflyOf(array) {
    const match = /with butterfly (0x[0-9a-f]+)/.exec(describe(array));
    if (!match)
        throw new Error("describe() names no butterfly: " + describe(array));
    return parseInt(match[1], 16);
}

const makers = {
    ArrayWithInt32: index => index,
    ArrayWithDouble: index => index + 0.5,
    ArrayWithContiguous: index => "s" + index,
};

const length = 2000;
const count = 40;
for (const [mode, make] of Object.entries(makers)) {
    const array = [];
    for (let index = 0; index < length; ++index)
        array.push(make(index));
    // fastUnshift() asks for room behind the last element.
    array.push(make(length));
    array.pop();
    shouldBe($vm.indexingMode(array), mode, mode + ": indexing mode");

    // The first unshift() allocates room in front of the elements. Each later one takes a slot of it.
    array.unshift(make(-1));
    let start = butterflyOf(array);
    for (let done = 2; done <= count; ++done) {
        array.unshift(make(-done));
        shouldBe(butterflyOf(array), start - 8, mode + ": the start of the elements after " + done + " calls");
        start -= 8;
    }

    shouldBe($vm.indexingMode(array), mode, mode + ": indexing mode afterwards");
    shouldBe(array.length, length + count, mode + ": length");
    for (let index = 0; index < length + count; ++index)
        shouldBe(array[index], make(index - count), mode + ": element " + index);
}
