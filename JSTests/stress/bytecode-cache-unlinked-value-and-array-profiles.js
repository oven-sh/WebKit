//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useConcurrentJIT=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=1000")
//@ runBytecodeCache("--useLazyUnlinkedValueAndArrayProfiles=0", "--useConcurrentJIT=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100")
//@ runBytecodeCache("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100", "--useEagerCodeBlockJettisonTiming=1")
//@ runBytecodeCache

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + " expected " + expected);
}

function makeFunctions(count) {
    let functions = [];
    for (let i = 0; i < count; ++i) {
        functions.push(new Function("array", "object", "value", `
            let sum = value + ${i};
            for (let j = 0; j < array.length; ++j)
                sum += array[j];
            if (object.k${i % 5} !== undefined)
                sum += object.k${i % 5};
            array[0] = sum & 15;
            return typeof sum === "number" ? sum | 0 : sum.length;
        `));
    }
    return functions;
}

function sum(array, object, value) {
    let total = value;
    for (let j = 0; j < array.length; ++j)
        total += array[j];
    if (object.k1 !== undefined)
        total += object.k1;
    array[0] = total & 15;
    return typeof total === "number" ? total | 0 : total.length;
}

let shapes = [{ k0: 1 }, { k1: 2, k0: 1 }, { k2: 3 }, { k3: 1.5 }, { k4: 4 }];
let arrays = [[1, 2, 3], [1.5, 2.5], [1, "x"].slice(0, 1), new Int32Array([4, 5])];

function drive(functions, rounds) {
    let total = 0;
    for (let round = 0; round < rounds; ++round) {
        for (let i = 0; i < functions.length; ++i)
            total += functions[i](arrays[(i + round) % arrays.length], shapes[(i + round) % shapes.length], round % 7 ? round : round + 0.5);
        total += sum(arrays[round % arrays.length], shapes[round % shapes.length], round);
    }
    return total;
}

let functions = makeFunctions(20);
let first = drive(functions, 30);
fullGC();
let second = drive(functions, testLoopCount / 10);
fullGC();
edenGC();
shouldBe(typeof first, "number");
shouldBe(typeof second, "number");
shouldBe(sum([1, 2], { k1: 3 }, 4), 10);
shouldBe(sum([1, 2], {}, "a"), 3);
