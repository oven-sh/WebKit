if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    // The microtask queue stays in the VM from the first job to the last. Between two jobs no JavaScript is on the stack,
    // and a job is often over before the thread is suspended.
    runAsyncTest(async () => {
        startSamplingProfiler({ intervalInMicroseconds: 200, recordsSamplesWithoutFrames: true });
        await new Tally().repeatAsyncUntil(tally => tally.Program >= 5 && tally.JavaScript >= 5, async () => {
            for (let i = 0; i < testLoopCount; i++)
                await undefined;
        });
    });
}
