//@ runDefault("--validateAbstractInterpreterState=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")

// The AI-state validator dumps the whole graph. Loop unrolling clones the loop body,
// CFG simplification then merges the block a clone was cloned from, and the dump of
// that clone has to survive it.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

function hot(i, late) {
    let s = 0;
    for (let j = 0; j < 4; j++) {
        s += j * i;
        if (late && j === 1)
            continue;
        s ^= j;
    }
    return s;
}

// The same arithmetic written out, so this one has no loop to unroll.
function reference(i, late) {
    let s = i;
    if (!late)
        s = s ^ 1;
    s = (s + 2 * i) ^ 2;
    return (s + 3 * i) ^ 3;
}

// Turn the branch on late in the run, after the function reaches the FTL.
const switchAt = testLoopCount - (testLoopCount >> 2);

let result = 0;
let expected = 0;
for (let i = 0; i < testLoopCount; i++) {
    result = (result + hot(i, i > switchAt)) | 0;
    expected = (expected + reference(i, i > switchAt)) | 0;
}

shouldBe(result, expected);
