//@ runDefault("--useConcurrentJIT=false")
//@ runDefault("--useConcurrentJIT=false", "--maximumOptimizationCandidateBytecodeCost=10")
//@ runDefault("--useDFGJIT=false")
//@ runDefault("--useJIT=false")

// Only the optimizing compilers read the predictions of value profiles. Code they are never going to compile (it is too
// large for them, or they are off) keeps its samples in the profiles' buckets and never gets the table of predictions.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: ${String(actual)}, expected ${String(expected)}`);
}


function hot(o, n) {
    let sum = 0;
    for (let i = 0; i < n; ++i)
        sum += o.a + o.b.length;
    return sum;
}
noInline(hot);

let result = 0;
for (let i = 0; i < testLoopCount; ++i)
    result = hot({ a: i % 3 ? 1 : 1.5, b: "xy" }, 10);
shouldBe(result, (((testLoopCount - 1) % 3) ? 1 : 1.5) * 10 + 20, "result");
fullGC();
for (let i = 0; i < 100; ++i)
    result = hot({ a: 2, b: "xyz" }, 10);
fullGC();
shouldBe(result, 50, "result after the collections");
// The second run line makes every function too large, the last two have no optimizing compiler.
const wasOptimized = $vm.useDFGJIT() && numberOfDFGCompiles(hot) > 0; // (Without the compiler numberOfDFGCompiles() pretends.)
shouldBe($vm.hasValueProfilePredictions(hot), wasOptimized, "hot() has predictions exactly when a compiler read them");
