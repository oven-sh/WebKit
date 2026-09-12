//@ runDefault("--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")

// Compiler threads read the storage of constant objects (to fold loads of their properties and elements) while the
// mutator evacuates it and, with evacuateAuxiliaryBlocksAfterEveryFullCollection, scribbles over the old copies. They are
// parked at a safepoint while cells move.

const holders = [];
for (let h = 0; h < 20; ++h) {
    const object = {};
    for (let i = 0; i < 20; ++i)
        object["k" + i] = h * 100 + i;
    object.array = [];
    for (let i = 0; i < 30; ++i)
        object.array.push(h + i);
    object.doubles = [h + 0.5, 1.5, 2.5];
    holders.push(object);
    globalThis["H" + h] = object;
}
function garbage() {
    for (let i = 0; i < 100; ++i) {
        [i, i, i, i];
        ({ a: i, b: i, c: i, d: i, e: i, f: i, g: i, h: i, i: i });
    }
}
noInline(garbage);
let moved = 0;
const generations = Math.max(20, Math.min(40, testLoopCount / 100));
for (let generation = 0; generation < generations; ++generation) {
    const h = generation % 20;
    const f = new Function("n", `let s = 0; const o = H${h}; for (let i = 0; i < n; ++i) { s += o.k11 + o.k19 + o.k${generation % 20} + o.array[i % 30] + o.array.length + o.doubles[1] + H${(h + 7) % 20}.k15; } return s + ${generation};`);
    let expected = generation;
    for (let i = 0; i < 50; ++i) {
        const object = holders[h];
        expected += object.k11 + object.k19 + object["k" + (generation % 20)] + object.array[i % 30] + object.array.length + object.doubles[1] + holders[(h + 7) % 20].k15;
    }
    for (let repeat = 0; repeat < 150; ++repeat) {
        const result = f(50);
        if (result !== expected)
            throw new Error("generation " + generation + ", repeat " + repeat + ": " + result + ", expected " + expected);
        if (repeat % 25 === 24) {
            garbage();
            gc();
            moved += $vm.evacuateAuxiliaryBlocks(1).movedCells;
        }
    }
}
