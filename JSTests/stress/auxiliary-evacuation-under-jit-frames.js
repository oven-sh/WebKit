//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--useConcurrentGC=0")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--useConcurrentJIT=0", "--useFTLJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20")

// With evacuateAuxiliaryBlocksAfterEveryFullCollection the first allocation that takes the slow path after a full
// collection evacuates every Auxiliary block. Here that allocation sits in the middle of compiled loops that have loaded
// an array's or an object's storage pointer once and keep using it: the storage must be found in their registers or
// spill slots and left in place. (Without the scan of the stack and the registers the sums are wrong or the stores are lost.)

let sink;
function makeNumbers(count, seed) {
    const array = [];
    for (let i = 0; i < count; ++i)
        array.push((i * 7919 + seed) % 1009);
    return array;
}
function sumWithAllocation(array) {
    let sum = 0;
    for (let i = 0; i < array.length; ++i) {
        sum += array[i];
        sink = { i };
        sum += array[i];
    }
    return sum;
}
noInline(sumWithAllocation);
function loadPropertiesWithAllocation(object) {
    let sum = 0;
    for (let i = 0; i < 50; ++i) {
        sum += object.k11 + object.k19;
        sink = [i];
        sum += object.k12 + object.k15;
    }
    return sum;
}
noInline(loadPropertiesWithAllocation);
function storePropertiesWithAllocation(object) {
    for (let i = 0; i < 50; ++i) {
        object.k12 = i;
        sink = [i];
        object.k13 = i + 1;
    }
}
noInline(storePropertiesWithAllocation);
function storeFunctionPropertiesWithAllocation(object) {
    for (let i = 0; i < 50; ++i) {
        object.k12 = i;
        sink = [i];
        object.k13 = i + 1;
    }
}
noInline(storeFunctionPropertiesWithAllocation);

const numbers = makeNumbers(100, 3);
let expectedSum = 0;
for (const value of numbers)
    expectedSum += 2 * value;
const wide = {};
for (let k = 0; k < 20; ++k)
    wide["k" + k] = k;
const plain = [], functions = [];
for (let n = 0; n < 8; ++n) {
    const object = {};
    for (let k = 0; k < 22; ++k)
        object["k" + k] = k + n;
    plain.push(object);
    const f = function () { };
    for (let k = 0; k < 16; ++k)
        f["k" + k] = k + n;
    functions.push(f);
}
function garbage() {
    for (let i = 0; i < 200; ++i) {
        sink = [i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7];
        sink = { a: i, b: i, c: i, d: i, e: i, f: i, g: i, h: i, i: i, j: i, k: i, l: i, m: i, n: i, o: i, p: i, q: i, r: i, s: i, t: i, u: i, v: i };
    }
}
noInline(garbage);
function collect() {
    garbage();
    gc();
}

const iterations = Math.max(300, Math.min(3000, testLoopCount));
const period = Math.floor(iterations / 30);
for (let i = 0; i < iterations; ++i) {
    const collectNow = i % period === period - 1;
    const n = i % 8;
    if (collectNow)
        collect();
    const sum = sumWithAllocation(numbers);
    if (sum !== expectedSum)
        throw new Error("sumWithAllocation, iteration " + i + ": " + sum + ", expected " + expectedSum);
    if (collectNow)
        collect();
    const loaded = loadPropertiesWithAllocation(wide);
    if (loaded !== 50 * (11 + 19 + 12 + 15))
        throw new Error("loadPropertiesWithAllocation, iteration " + i + ": " + loaded);
    plain[n].k12 = plain[n].k13 = functions[n].k12 = functions[n].k13 = -1;
    if (collectNow)
        collect();
    storePropertiesWithAllocation(plain[n]);
    if (plain[n].k12 !== 49 || plain[n].k13 !== 50 || plain[n].k21 !== 21 + n)
        throw new Error("storePropertiesWithAllocation, iteration " + i + ": " + plain[n].k12 + " " + plain[n].k13 + " " + plain[n].k21);
    if (collectNow)
        collect();
    storeFunctionPropertiesWithAllocation(functions[n]);
    if (functions[n].k12 !== 49 || functions[n].k13 !== 50 || functions[n].k15 !== 15 + n)
        throw new Error("storeFunctionPropertiesWithAllocation, iteration " + i + ": " + functions[n].k12 + " " + functions[n].k13 + " " + functions[n].k15);
}
