//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useConcurrentJIT=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=1000", "--reportCompileTimes=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useConcurrentJIT=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100", "--useSamplingProfiler=1", "--sampleInterval=30")
//@ runBytecodeCache("--useConcurrentJIT=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100", "--useSamplingProfiler=1", "--sampleInterval=30", "--reportCompileTimes=1")

// Functions decoded from the bytecode cache keep their name in the cache payload until the mutator first asks for it
// (useThinChildExecutables). Compiler threads print CodeBlock names when a plan completes (reportCompileTimes), and the
// sampling profiler resolves frame line/column when the collector visits it and frame names when it reports. None of
// those may materialize (atomize) a deferred name off the mutator; they must either print a placeholder or go through
// the WithoutGC/Concurrently accessors. Most functions below are only ever called, never introspected, so their names
// stay deferred for the whole run.

function makeFamily(seed) {
    // Distinctly named closures so every tier sees fresh executables whose names nobody asked for.
    function leafAdd(a, b) { return a + b + seed; }
    function leafMul(a, b) { return a * b + seed; }
    let arrowLeaf = (x) => leafAdd(x, 1) ^ leafMul(x, 2);
    function middle(x) { let s = 0; for (let i = 0; i < 3; ++i) s += arrowLeaf(x + i); return s; }
    class Shape {
        constructor(w) { this.w = w; }
        area() { return middle(this.w) + this.w; }
        static make(w) { return new Shape(w); }
        get twice() { return this.area() * 2; }
    }
    let anonymous = function (x) { return Shape.make(x).twice + middle(x); };
    return { run(x) { return anonymous(x) + leafAdd(x, x); }, thrower(x) { if (x === -1) throw new Error("no"); return x; } };
}

let families = [];
for (let s = 0; s < 6; ++s)
    families.push(makeFamily(s));

let total = 0;
for (let i = 0; i < 40 * testLoopCount; ++i) {
    let f = families[i % families.length];
    total += f.run(i & 255);
    total += f.thrower(i & 7);
    if (i % 50000 === 0) {
        // Let some CodeBlocks die with compilations in flight and errors pending, then keep going with new ones.
        let errors = [];
        for (let j = 0; j < 5; ++j) {
            try { f.thrower(-1); } catch (e) { errors.push(e); }
        }
        families[i % families.length] = makeFamily(i);
        edenGC();
        if (i % 100000 === 0)
            fullGC();
        for (let e of errors)
            String(e.stack);
    }
}
if (typeof total !== "number")
    throw new Error("bad total");

// Resolves every sampled frame's name and position (SamplingProfiler::StackFrame::displayName, lineColumn).
if (jscOptions().useSamplingProfiler) {
    let traces = samplingProfilerStackTraces();
    let names = new Set();
    for (let trace of traces.traces)
        for (let frame of trace.frames)
            names.add(frame.name);
    if (jscOptions().dumpOptions) // handy when running by hand: --dumpOptions=1
        print("sampled frame names: " + [...names].join(", "));
}
