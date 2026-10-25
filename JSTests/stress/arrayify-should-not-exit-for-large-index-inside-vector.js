//@ skip if not $jitTests
//@ runDefault("--validateOptions=true", "--useConcurrentJIT=0")
//@ runDefault("--validateOptions=true", "--useConcurrentJIT=0", "--useFTLJIT=0")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

let object = { };
let count = testLoopCount * 10;
let length = 100016;
let index = 100008;

let int32Array = [];
for (let i = 0; i < length; ++i)
    int32Array.push(i);

function makeInt32Array() {
    return int32Array.slice();
}

function putObject(array, index, value) {
    array[index] = value;
}
noInline(putObject);

function putDouble(array, index, value) {
    array[index] = value;
}
noInline(putDouble);

function read(array, index) {
    return array[index];
}
noInline(read);

for (let i = 0; i < count; ++i) {
    putObject([1, 2, 3], 1, i);
    putObject([object, object], 1, object);
    putDouble([1, 2, 3], 1, i);
    putDouble([1.5, 2.5], 1, 3.5);
    shouldBe(read([1, 2, 3], 1), 2);
    shouldBe(read([object, object], 1), object);
}

// These indices are large, but they are inside the vector. So the arrays are converted in the optimized code.
let readCompiles = numberOfDFGCompiles(read);
let array = makeInt32Array();
for (let i = 0; i < count; ++i)
    shouldBe(read(array, index), index);
shouldBe(array.length, length);
shouldBe(numberOfDFGCompiles(read), readCompiles);

for (let i = 0; i < 8; ++i) {
    array = makeInt32Array();
    shouldBe($vm.indexingMode(array), "ArrayWithInt32");
    putObject(array, index, object);
    shouldBe($vm.indexingMode(array), "ArrayWithContiguous");
    shouldBe(array[index], object);
    shouldBe(array[index - 1], index - 1);
    shouldBe(array.length, length);

    array = makeInt32Array();
    putDouble(array, index, 0.5);
    shouldBe($vm.indexingMode(array), "ArrayWithDouble");
    shouldBe(array[index], 0.5);
    shouldBe(array[index - 1], index - 1);
    shouldBe(array.length, length);
}
