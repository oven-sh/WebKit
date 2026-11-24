//@ defaultNoEagerRun
//@ runDefault("--useDFGJIT=0")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForFTLOptimizeAfterWarmUp=1000")

// These C functions cannot tell the VM which frame called them.
if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    const doubles = Array.from({ length: 2000 }, (_, i) => i + 0.5);
    const bigInts = [1n << 100000n, (1n << 100000n) + 1n];
    const source = new Float64Array(2000), target = new Float64Array(2001);
    const keys = Object.keys({ ["a".repeat(5000) + "b"]: 0, ["a".repeat(5000) + "c"]: 0 });
    const operations = [
        function callSin(i) { return Math.sin(i + 0.5); },
        function callPow(i) { return Math.pow(i + 0.5, 0.3); },
        function callPowInt32(i) { return Math.pow(1.0001, 2000 + i); },
        function callMod(i) { return (i * 1e200) % 3.7; },
        function callIndexOf(i) { return doubles.indexOf(-0.5 - i); },
        function callCompareBigInts(i) { return bigInts[i & 1] < bigInts[~i & 1]; },
        function callCompareIdentifiers(i) { return keys[i & 1] < keys[~i & 1]; },
        function callSet(i) { target.set(source, i & 1); return 0; },
        function callNow() { return Date.now(); },
    ];

    // The conversion is made by a function that does tell, so that this is the last frame to be known.
    const object = { };
    function caller(loop, count) {
        String(object);
        return loop(count);
    }
    noInline(caller);

    startSamplingProfiler({ intervalInMicroseconds: 50, recordsSamplesWithoutFrames: true });
    function stepThatCalls(operation) {
        const loop = new Function("operation", `return function loop(count) {
            let result = 0; // ${operation.name} is inlined.
            for (let i = 0; i < count; i++)
                result += operation(i);
            return result;
        }`)(operation);
        noInline(loop);
        for (let i = 0; i < testLoopCount; i++)
            caller(loop, 3);
        return sampleOnly(() => {
            for (const end = preciseTime() + 0.002; preciseTime() < end;)
                caller(loop, 1000);
        });
    }
    for (const operation of operations)
        expectMostStacksToInclude([operation.name, "loop", "caller"], stepThatCalls(operation));

    const repeat = loop => sampleOnly(() => {
        for (const end = preciseTime() + 0.002; preciseTime() < end;)
            caller(loop, 1000);
    });

    // Without the optimizing compilers it is a thunk that makes this call.
    if (!$vm.useDFGJIT()) {
        const { exp } = Math;
        function callExp(count) {
            let result = 0;
            for (let i = 0; i < count; i++)
                result += exp(-740.5) + exp(-741.5) + exp(-742.5);
            return result;
        }
        for (let i = 0; i < testLoopCount; i++)
            caller(callExp, 3);
        for (let tally; !(tally?.withTopFrame("exp") > tally?.withStackIncluding("callExp") / 3);) {
            failIfOutOfTime(() => tally);
            tally = new Tally().repeatUntil(tally => tally.withStackIncluding("callExp") >= 60, repeat(callExp));
        }
    }

    // The code that enters the VM has a frame of its own kind. A walk that does not know it fails.
    const unsorted = Float64Array.from({ length: 2000 }, (_, i) => (i * 7919) % 2003);
    function callSort() { return unsorted.slice().sort((a, b) => a - b).length; }
    noInline(callSort);
    for (let tally; !(tally?.Program + tally?.withStack("(program)") < tally?.withStackIncluding("callSort", "caller") / 50);) {
        failIfOutOfTime(() => tally);
        tally = new Tally().repeatUntil(tally => tally.withStackIncluding("callSort", "caller") >= 500, repeat(callSort));
    }

    // A function that never returns can only be compiled as it is entered, here by the second call.
    if ($vm.useJIT() && jscOptions().thresholdForJITAfterWarmUp === 10) {
        function callLarge(large) {
            String(object);
            try {
                large(0);
            } catch { }
        }
        noInline(callLarge);
        const small = Array.from({ length: 50 }, (_, i) => new Function(`return ${i};`));
        for (let i = 0; i < testLoopCount; i++)
            callLarge(small[i % small.length]);
        let large, count = 0;
        new Tally().repeatUntil(tally => tally.withStackIncluding("large", "callLarge") > 30 + tally.withTopFrame("callLarge"), sampleOnly(() => callLarge(large), () => {
            large = new Function(`return function large(x) { if (x) { ${"x = x.y + x.z;".repeat(1000)} } throw 0; } // ${count++}`)();
            callLarge(large);
        }));
    }
}
