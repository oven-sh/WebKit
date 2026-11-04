//@ skip if !$aot
//@ runDefault("--useAOT=true", "--markedBlockCageSizeInMB=32")
//@ runDefault("--useAOT=true")
//@ runDefault("--useAOT=true", "--markedBlockCageSizeInMB=48")
//@ runDefault("--useAOT=true", "--markedBlockCageSizeInMB=16")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

const objectCount = 400000;
const arrayCount = 20000;
const arrayLength = 40;
const groups = [];

function allocateGroup(seed) {
    const objects = [];
    for (let i = 0; i < objectCount; ++i)
        objects.push({ first: i, second: seed, third: i + seed });
    const arrays = [];
    for (let i = 0; i < arrayCount; ++i) {
        const array = new Array(arrayLength);
        for (let j = 0; j < arrayLength; ++j)
            array[j] = i + j + seed;
        arrays.push(array);
    }
    groups.push({ objects, arrays });
}

function dropEverySecond(groupIndex) {
    const { objects, arrays } = groups[groupIndex];
    for (let i = 1; i < objectCount; i += 2)
        objects[i] = null;
    for (let i = 1; i < arrayCount; i += 2)
        arrays[i] = null;
}

function checkSum(groupIndex, seed, stride) {
    const { objects, arrays } = groups[groupIndex];
    let sum = 0;
    let expectedSum = 0;
    for (let i = 0; i < objectCount; i += stride) {
        const object = objects[i];
        sum += object.first + object.second + object.third;
        expectedSum += 2 * (i + seed);
    }
    for (let i = 0; i < arrayCount; i += stride) {
        const array = arrays[i];
        shouldBe(array.length, arrayLength);
        for (let j = 0; j < arrayLength; ++j)
            sum += array[j];
        expectedSum += arrayLength * (i + seed) + arrayLength * (arrayLength - 1) / 2;
    }
    shouldBe(sum, expectedSum);
}

allocateGroup(1);
checkSum(0, 1, 1);

dropEverySecond(0);
fullGC();
allocateGroup(2);
checkSum(0, 1, 2);
checkSum(1, 2, 1);

groups.length = 0;
fullGC();
fullGC();
allocateGroup(3);
checkSum(0, 3, 1);
