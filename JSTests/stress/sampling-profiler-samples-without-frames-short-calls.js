//@ defaultNoEagerRun

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    // The microtask queue stays in the VM from the first job to the last. Between two jobs no JavaScript is on the stack,
    // and a job is often over before the thread is suspended.
    function job() { }
    function queueJobs(withJob) {
        let promise = Promise.resolve();
        for (let i = 0; i < 1000; i++) {
            if (withJob)
                promise = promise.then(job);
            promise = promise.then().then().then();
        }
        startSamplingProfiler();
        return promise.then(pauseSamplingProfiler);
    }

    runAsyncTest(async () => {
        startSamplingProfiler({ intervalInMicroseconds: 50, recordsSamplesWithoutFrames: true });
        pauseSamplingProfiler();
        const tally = await new Tally().repeatAsyncUntil(tally => tally.Program >= 5 && tally.withStack("job") >= 5, () => queueJobs(true));
        assert(!tally.withStack("(no frames)"), "a function was lost: " + tally);

        // What was called last is not on the stack any more.
        await new Tally().repeatAsyncUntil(tally => {
            assert(!tally.withStack("job"), "it was not called: " + tally);
            return tally.Program >= 20;
        }, () => queueJobs(false));

        // It is the function of that instant which counts, not the last one to be called before the thread is suspended.
        let result = 0;
        function long() {
            for (let i = 0; i < 2000; i++)
                result = (result * 31 + i) | 0;
        }
        function brief() { }
        for (let tally; !(tally?.withStack("brief") < tally?.withStack("long") / 10);) {
            failIfOutOfTime(() => tally);
            tally = await new Tally().repeatAsyncUntil(tally => tally.withStack("long") + tally.withStack("brief") >= 100, () => {
                let promise = Promise.resolve();
                for (let i = 0; i < 30; i++) {
                    promise = promise.then(long).then(brief);
                    for (let j = 0; j < 300; j++)
                        promise = promise.then();
                }
                startSamplingProfiler();
                return promise.then(pauseSamplingProfiler);
            });
        }
    });
}
