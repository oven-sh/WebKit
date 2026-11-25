//@ runDefault("--forceSamplingProfilerToMissStacks=1", "--useSamplingProfiler=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--useGenerationalGC=0", "--collectContinuously=1", "--scribbleFreeCells=1", "--sweepSynchronously=1")

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    startSamplingProfiler({ intervalInMicroseconds: 10, recordsSamplesWithoutFrames: true });

    // A program is only known by its code, which is left alone while the world is stopped.
    new Tally().repeatUntil(tally => {
        assert(!tally.withStack("(unknown)"), "it has no name to show: " + tally);
        return tally.withStack("(program)") && tally.withStack("(no frames)");
    }, sampleOnly(() => flashHeapAccess(2)));

    // The code that was entered dies while the function is still running.
    let count = 0;
    function call(callback) { callback?.(); }
    noInline(call);
    function optimizedFunction() {
        pauseSamplingProfiler();
        const outermost = new Function("call", `
            let constant = ${count++};
            function assign() { constant++; }
            function jettisonCaller(callback) { if (callback) assign(); }
            noInline(jettisonCaller);
            return function outermost(callback) { jettisonCaller(callback); const result = constant; call(callback); return result; }`)(call);
        noInline(outermost);
        for (let i = 0; i < 300; i++)
            outermost();
        releaseSamplingProfilerSamples();
        return outermost;
    }
    function enter(outermost, callback) {
        // The last call is made from a cache, which lets go of the code.
        Promise.resolve().then(outermost);
        Promise.resolve().then(new Function(`// ${count++}`));
        const promise = Promise.resolve(callback).then(outermost);
        startSamplingProfiler();
        return promise;
    }

    runAsyncTest(async () => {
        // The collector does not need the thread to find out.
        for (let i = 0, named = 0, unnamed = 0; i < 10 || named < 2 * unnamed; i++) {
            failIfOutOfTime(() => `${named} named, ${unnamed} unnamed`);
            const outermost = optimizedFunction();
            const tally = new Tally;
            await enter(outermost, () => {
                for (let i = 0; i < 6; i++)
                    flashHeapAccess(2);
            });
            tally.update();
            named += tally.withStack("outermost");
            unnamed += tally.Program + tally.withStack("(no frames)");
        }

        // Other code takes its place.
        const runFunction = () => new Function(`// ${count++}`)();
        const runEval = () => (0, eval)(`// ${count++}`);
        const sampledFrames = () => {
            pauseSamplingProfiler();
            return samplingProfilerStackTraces().traces.flatMap(trace => trace.frames).filter(frame => frame.name === "outermost");
        };
        for (let compared = 0; compared < 6;) {
            failIfOutOfTime(() => compared);
            const runOther = compared % 2 ? runEval : runFunction;
            const outermost = optimizedFunction();
            await enter(outermost, () => {
                fullGC();
                for (let i = 0; i < 50; i++)
                    runOther();
                spin(0.5);
            });
            const withDeadCode = sampledFrames();
            releaseSamplingProfilerSamples();
            await enter(outermost, () => spin(0.5));
            const withLiveCode = sampledFrames();
            assert([...withDeadCode, ...withLiveCode].every((frame, _, [first]) => frame.sourceID === first.sourceID), "it has the source of another function");
            compared += withDeadCode.length && withLiveCode.some(frame => frame.category !== "Unknown Executable");
        }
    });
}
