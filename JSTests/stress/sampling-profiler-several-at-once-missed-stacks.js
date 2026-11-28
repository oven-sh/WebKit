//@ runDefault("--forceSamplingProfilerToMissStacks=1")

// The VM notes what it enters for as long as it has a profiler.
if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    const create = () => {
        const profiler = createSamplingProfiler({ intervalInMicroseconds: 100, recordsSamplesWithoutFrames: true });
        return Object.assign(new Tally(profiler.releaseSamples), { shutdown: profiler.shutdown });
    };
    const sample = tally => tally.repeatUntil(tally => tally.stackTraces + tally.Program >= 10, () => spin(0.2));

    const first = create();
    let second;
    runAsyncTest(() => Promise.resolve().then(function createsAnother() {
        second = create();
        sample(second);
        assert(second.withStack("createsAnother") && !second.Program, "forgotten: " + second);
        first.shutdown();
    }).then(function afterOneHasShutDown() {
        const third = create();
        sample(third);
        assert(third.withStack("afterOneHasShutDown") && third.stacks.size === 1, "not noted: " + third);
        second.shutdown();
        third.shutdown();
    }).then(function enteredWithoutProfiler() {
        const fourth = create();
        sample(fourth);
        assert(!fourth.stackTraces, "taken for another: " + fourth);
    }));
}
