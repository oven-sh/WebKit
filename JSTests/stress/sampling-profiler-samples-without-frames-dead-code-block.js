//@ runDefault("--forceSamplingProfilerToMissStacks=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--useGenerationalGC=0", "--collectContinuously=1", "--scribbleFreeCells=1", "--sweepSynchronously=1")

// The code that was entered dies while the function is still running, and the collector does not need the thread to find out.
if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    function wait(resolve) {
        if (!resolve)
            return;
        for (let i = 0; i < 6; i++)
            flashHeapAccess(2);
        resolve();
    }
    noInline(wait);

    startSamplingProfiler({ intervalInMicroseconds: 10, recordsSamplesWithoutFrames: true });
    runAsyncTest(async () => {
        for (let i = 0, named = 0, unnamed = 0; i < 10 || named < 2 * unnamed; i++) {
            failIfOutOfTime(() => `${named} named, ${unnamed} unnamed`);
            pauseSamplingProfiler();
            const outermost = new Function("wait", `
                let constant = ${i};
                function assign() { constant++; }
                function jettisonCaller(resolve) { if (resolve) assign(); }
                noInline(jettisonCaller);
                return function outermost(resolve) { jettisonCaller(resolve); const result = constant; wait(resolve); return result; }`)(wait);
            noInline(outermost);
            for (let i = 0; i < 300; i++)
                outermost();

            const tally = new Tally;
            await new Promise(resolve => {
                // The last call is made from a cache, which lets go of the code.
                Promise.resolve().then(outermost);
                Promise.resolve().then(new Function(`// ${i}`));
                Promise.resolve(resolve).then(outermost);
                startSamplingProfiler();
            });
            tally.update();
            named += tally.withStack("outermost");
            unnamed += tally.Program + tally.withStack("(no frames)");
        }
    });
}
