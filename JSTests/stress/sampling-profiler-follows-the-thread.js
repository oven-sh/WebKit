//@ defaultNoEagerRun

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    // It calls nothing, so only the registers of the right thread tell that it is running.
    function loop() {
        let result = 0;
        for (let i = 0; i < 50000; i++)
            result = (result * 31 + i) | 0;
        return result;
    }
    noInline(loop);
    const callers = [function before() { return loop() + 1; }, function during() { return loop() + 1; }, function after() { return loop() + 1; }];
    const [before, during, after] = callers;
    callers.forEach(noInline);
    for (let i = 0; i < 20; i++)
        loop();

    let samples = { stackTraces: [], withoutFrames: [] };
    new Tally(() => samples).repeatUntil(tally => callers.every(({ name }) => tally.withStackIncluding("loop", name) >= 20), () => {
        const profiler = createSamplingProfiler({ intervalInMicroseconds: 50, recordsSamplesWithoutFrames: true });
        before();
        callOnAnotherThread(during);
        after();
        samples = profiler.releaseSamples();
        profiler.shutdown();
    });
}
