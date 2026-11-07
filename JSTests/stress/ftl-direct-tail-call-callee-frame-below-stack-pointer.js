//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=400")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=400", "--useSamplingProfiler=1", "--sampleInterval=50")

// A direct tail call from the FTL must not write the callee's frame below the
// stack pointer. The OS builds a signal frame there whenever a signal arrives,
// which overwrites the arguments that are already in place. Where threads are
// suspended with a signal, the sampling profiler sends one per sample.
//
// CallFrameShuffler::prepareForTailCall asserts this when it generates the
// code, so a build with assertions fails on every run, not once in a while.

"use strict";

function sum() {
    let t = arguments.length;
    for (let i = 0; i < arguments.length; i++)
        t = (t * 31 + arguments[i]) % 1000003;
    return t;
}
noInline(sum);

function test(f) {
    noInline(f);

    // Below the FTL: gives the call site a profile, and the result to expect.
    const expected = f(0, true);

    for (let i = 0; i < testLoopCount; i++)
        f(i, false);

    for (let i = 0; i < 20; i++) {
        const got = f(0, true);
        if (got !== expected)
            throw new Error(`${f.name}: got ${got} from the FTL, expected ${expected}`);
    }
}

const values = Array.from({ length: 1000 }, (_, i) => (i * 7 + 3) % 11);

// More arguments than the caller's own frame has slots.
test(new Function("sum", `"use strict"; return function few(x, rare) { if (rare) return sum(${values.slice(0, 64).join(",")}); return x + 1; }`)(sum));

// Spreads of an array literal: the arguments elimination phase turns the
// varargs tail call into one with a static argument count, here 60000.
test(new Function("sum", `"use strict"; return function many(x, rare) { if (rare) { const a = [${values.join(",")}]; return sum(${Array(60).fill("...a").join(",")}); } return x + 1; }`)(sum));
