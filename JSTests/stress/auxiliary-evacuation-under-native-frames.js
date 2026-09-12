//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--useConcurrentGC=0")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--useConcurrentGC=0", "--useJIT=0")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--collectContinuously=1")

// With evacuateAuxiliaryBlocksAfterEveryFullCollection every full collection is followed by an evacuation of every
// Auxiliary block, wherever the mutator is: inside natives that keep a pointer to an array's storage in a local across
// the call that collects, and inside JIT code that has hoisted the load of a butterfly. Storage that the stack refers to
// must stay put; everything else moves and the old copy is scribbled.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}
function collect() {
    // Fill the blocks around what is live with garbage first, so that there is something to move.
    for (let i = 0; i < 200; ++i)
        [i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7];
    gc();
}
noInline(collect);

function makeNumbers(count, seed) {
    const array = [];
    for (let i = 0; i < count; ++i)
        array.push((i * 7919 + seed) % 1009);
    return array;
}
function isSorted(array) {
    for (let i = 1; i < array.length; ++i) {
        if (array[i - 1] > array[i])
            return false;
    }
    return true;
}

for (let round = 0; round < 6; ++round) {
    // Array.prototype.sort with a comparator that collects.
    {
        const array = makeNumbers(300, round);
        let calls = 0;
        array.sort((a, b) => { if (!(++calls % 97)) collect(); return a - b; });
        assert(array.length === 300 && isSorted(array), "sort");
    }
    // toSorted / toSpliced / with
    {
        const array = makeNumbers(200, round + 1);
        let calls = 0;
        const sorted = array.toSorted((a, b) => { if (!(++calls % 61)) collect(); return a - b; });
        assert(sorted.length === 200 && isSorted(sorted) && array.length === 200, "toSorted");
    }
    // Array.from with a mapper, map, filter, forEach, reduce, flatMap.
    {
        const source = makeNumbers(150, round + 2);
        const mapped = Array.from(source, (value, index) => { if (!(index % 50)) collect(); return value * 2; });
        for (let i = 0; i < 150; ++i)
            assert(mapped[i] === source[i] * 2, "Array.from " + i);
        const again = source.map((value, index) => { if (index === 75) collect(); return [value, index]; });
        assert(again[149][1] === 149 && again[0][0] === source[0], "map");
        const flat = source.flatMap((value, index) => { if (index === 10) collect(); return [value, value + 1]; });
        assert(flat.length === 300 && flat[299] === source[149] + 1, "flatMap");
        const filtered = source.filter((value, index) => { if (index === 100) collect(); return value % 2; });
        assert(filtered.every((value) => value % 2), "filter");
        const sum = source.reduce((sum, value, index) => { if (index === 5) collect(); return sum + value; }, 0);
        let expected = 0;
        for (const value of source)
            expected += value;
        assert(sum === expected, "reduce");
    }
    // copyWithin / fill / splice / concat / slice on an array whose length getter or element conversion collects.
    {
        const array = makeNumbers(64, round + 3);
        const copy = array.slice();
        array.copyWithin({ valueOf() { collect(); return 8; } }, 0, 16);
        for (let i = 0; i < 16; ++i)
            assert(array[8 + i] === copy[i], "copyWithin " + i);
        array.fill(7, { valueOf() { collect(); return 60; } });
        assert(array[63] === 7 && array[59] === copy[59], "fill");
        const removed = array.splice({ valueOf() { collect(); return 2; } }, 3, "x", "y");
        assert(removed.length === 3 && array[2] === "x" && array[3] === "y" && array.length === 63, "splice");
        const spreadable = { length: 3, 0: "a", 1: "b", get 2() { collect(); return "c"; }, [Symbol.isConcatSpreadable]: true };
        const joined = array.concat(spreadable, [1, 2, 3]);
        assert(joined.length === 69 && joined[65] === "c" && joined[68] === 3, "concat");
    }
    // Objects with out-of-line properties: Object.assign / spread / JSON with getters and toJSON that collect.
    {
        const source = {};
        for (let k = 0; k < 20; ++k)
            source["k" + k] = k + round;
        Object.defineProperty(source, "getter", { enumerable: true, get() { collect(); return "got"; } });
        for (let k = 20; k < 30; ++k)
            source["k" + k] = k + round;
        const assigned = Object.assign({}, source);
        const spread = { ...source, extra: 1 };
        for (let k = 0; k < 30; ++k)
            assert(assigned["k" + k] === k + round && spread["k" + k] === k + round, "assign/spread k" + k);
        assert(assigned.getter === "got" && spread.getter === "got", "getter value");
        const holder = { list: makeNumbers(40, round), toJSON() { collect(); return { list: this.list, more: source.k5 }; } };
        const parsed = JSON.parse(JSON.stringify([holder, holder]));
        assert(parsed[1].list.length === 40 && parsed[0].more === 5 + round, "JSON");
    }
    // for-of / destructuring / spread over an array that grows while a collection happens in the loop body.
    {
        const array = makeNumbers(30, round + 4);
        let seen = 0;
        for (const value of array) {
            if (seen === 10) {
                collect();
                array.push(-1);
            }
            ++seen;
        }
        assert(seen === 31, "for-of saw the pushed element: " + seen);
        const [first, second, ...rest] = array;
        assert(rest.length === 29 && first === array[0] && second === array[1], "destructuring");
    }
}

// JIT code that keeps a butterfly in a register across a call that collects.
function sumWithCall(array, callee) {
    let sum = 0;
    for (let i = 0; i < array.length; ++i) {
        sum += array[i];
        if (i === 40)
            callee();
        sum += array[i];
    }
    return sum;
}
noInline(sumWithCall);
const hot = makeNumbers(100, 3);
let hotExpected = 0;
for (const value of hot)
    hotExpected += 2 * value;
for (let i = 0; i < 400; ++i)
    assert(sumWithCall(hot, i % 100 === 99 ? collect : () => { }) === hotExpected, "sumWithCall " + i);

function propertiesWithCall(object, callee) {
    let sum = object.k0 + object.k11;
    callee();
    return sum + object.k12 + object.k19;
}
noInline(propertiesWithCall);
const wide = {};
for (let k = 0; k < 20; ++k)
    wide["k" + k] = k;
for (let i = 0; i < 400; ++i)
    assert(propertiesWithCall(wide, i % 100 === 99 ? collect : () => { }) === 0 + 11 + 12 + 19, "propertiesWithCall " + i);
