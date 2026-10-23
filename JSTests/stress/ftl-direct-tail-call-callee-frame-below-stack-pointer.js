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

function expected(values) {
    let t = values.length;
    for (let i = 0; i < values.length; i++)
        t = (t * 31 + values[i]) % 1000003;
    return t;
}

function test(name, f, values) {
    noInline(f);
    const want = expected(values);

    // Once below the FTL, so that the call site has a profile.
    const first = f(3, true);
    if (first !== want)
        throw new Error(`${name}: got ${first} before the FTL, expected ${want}`);

    for (let i = 0; i < testLoopCount; i++) {
        if (f(i, false) !== i + 1)
            throw new Error(`${name}: wrong result on the common path`);
    }

    for (let i = 0; i < 20; i++) {
        const got = f(3, true);
        if (got !== want)
            throw new Error(`${name}: got ${got} from the FTL, expected ${want}`);
    }
}

// More arguments than the caller's own frame has slots.
{
    const values = Array.from({ length: 64 }, (_, i) => (i * 7 + 3) % 11);
    const f = new Function("sum", `"use strict"; return function few(x, rare) { if (rare) return sum(${values.join(",")}); return x + 1; }`)(sum);
    test("few", f, values);
}

// Spreads of array literals: the arguments elimination phase turns the varargs
// tail call into one with a static argument count, here 60000.
{
    const chunk = Array.from({ length: 1000 }, (_, i) => (i * 7 + 3) % 11);
    const spreads = 60;
    const f = new Function("sum", `"use strict"; return function many(x, rare) { if (rare) { const a = [${chunk.join(",")}]; return sum(${Array(spreads).fill("...a").join(",")}); } return x + 1; }`)(sum);
    test("many", f, Array(spreads).fill(chunk).flat());
}
