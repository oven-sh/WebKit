//@ skip if $buildType == "debug"
//@ runDefault("--useGenerationalGC=false", "--collectContinuously=true")

// JSArray::shiftCountWithAnyIndexingType / shiftCountWithArrayStorage / unshiftCount* / setLength clear the
// slots they vacate while the concurrent marker may be scanning the same butterfly. Those clears must be
// whole 8-byte stores. When they were compiled down to a libc memset() that uses narrower or unaligned
// stores (musl does), a marker thread could load a torn JSValue from a slot being cleared and crash.
// The heap here is made almost entirely of the arrays being spliced so that a full-heap mark spends its
// time in their butterflies while the mutator keeps clearing tails.

const seconds = 1.5;
const arrayCount = 2048;
const arrayLength = 1000;
const k = 3;

function fill(array) {
    for (let i = array.length; i < arrayLength; i++)
        array.push({ i });
}

const arrays = [];
for (let j = 0; j < arrayCount; j++) {
    const array = [];
    fill(array);
    arrays.push(array);
}

const deadline = Date.now() + seconds * 1000;
while (Date.now() < deadline) {
    for (const array of arrays) {
        array.splice(arrayLength - 2 * k, k);
        fill(array);
        array.length = arrayLength - k;
        fill(array);
    }
}

for (const array of arrays) {
    if (array.length !== arrayLength)
        throw new Error("bad length " + array.length);
    for (let i = arrayLength - 4 * k; i < arrayLength; i++) {
        if (typeof array[i] !== "object" || array[i] === null || typeof array[i].i !== "number")
            throw new Error("bad element at " + i);
    }
}
