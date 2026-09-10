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

let result = 0;
for (let i = 0; i < 3000; i++)
    result = (result + hot(i, i > 1800)) | 0;

shouldBe(result, 26986799);
