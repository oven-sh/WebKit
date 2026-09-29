//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--slowPathAllocsBetweenGCs=21", "--useBaselineJIT=0")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--useGenerationalGC=0", "--collectContinuously=1")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--slowPathAllocsBetweenGCs=11", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")

// Natives and JIT code that allocate while they hold a pointer to an array's or object's storage (slice, concat, spread,
// push, Object.assign, ...): with these options a full collection followed by an evacuation of every Auxiliary block
// happens inside those allocations. The storage they hold must be found on the stack and left in place. (Without that
// rule this test crashes.)

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function makeNumbers(count, seed) { const a = []; for (let i = 0; i < count; ++i) a.push((i * 7919 + seed) % 1009); return a; }
let total = 0;
const rounds = Math.max(20, Math.min(40, testLoopCount / 25));
for (let round = 0; round < rounds; ++round) {
    const source = makeNumbers(40 + (round % 17), round);
    const sliced = source.slice(3, 30);
    for (let i = 0; i < sliced.length; ++i) assert(sliced[i] === source[i + 3], "slice " + round);
    const joined = source.concat(sliced, [1, 2, 3], sliced);
    assert(joined.length === source.length + 2 * sliced.length + 3 && joined[source.length] === sliced[0] && joined[joined.length - 1] === sliced[sliced.length - 1], "concat " + round);
    const spread = [...source, ...sliced];
    assert(spread.length === source.length + sliced.length && spread[spread.length - 1] === sliced[sliced.length - 1], "spread " + round);
    const from = Array.from(source);
    assert(from.length === source.length && from[5] === source[5], "from " + round);
    const grown = [];
    for (let i = 0; i < 70; ++i) grown.push(source[i % source.length] + i);
    for (let i = 0; i < 70; ++i) assert(grown[i] === source[i % source.length] + i, "push " + round);
    const object = {};
    for (let k = 0; k < 24; ++k) object["k" + k] = k * round;
    const copy = { ...object };
    const assigned = Object.assign({ z: 1 }, object);
    for (let k = 0; k < 24; ++k) assert(copy["k" + k] === k * round && assigned["k" + k] === k * round, "props " + round);
    const doubles = source.map(Number).map((v) => v + 0.5);
    const rev = doubles.toReversed();
    assert(rev[0] === doubles[doubles.length - 1], "toReversed");
    const parsed = JSON.parse(JSON.stringify({ source, object }));
    assert(parsed.source.length === source.length && parsed.object.k23 === 23 * round, "json");
    const spliced = source.slice(); const removed = spliced.splice(2, 10, ...sliced);
    assert(removed.length === 10 && spliced.length === source.length - 10 + sliced.length && spliced[2] === sliced[0], "splice");
    const filled = new Array(50).fill(round); filled.length = 200; filled[199] = 1;
    assert(filled[49] === round && filled[199] === 1, "fill/grow");
    const unshifted = source.slice(); unshifted.unshift(1, 2, 3, 4, 5, 6, 7, 8);
    assert(unshifted[8] === source[0] && unshifted.length === source.length + 8, "unshift");
    total += joined.length;
}
