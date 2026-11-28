//@ defaultNoEagerRun

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    // They record samples without frames, so that a pause of the collector is not a stack trace.
    const create = (intervalInMicroseconds = stretchInSlowBuild(200)) => createSamplingProfiler({ intervalInMicroseconds, recordsSamplesWithoutFrames: true });
    const isInReport = ({ frames }) => frames[0] === "report";
    const hasAny = ({ stackTraces, withoutFrames }) => stackTraces.length || withoutFrames.length;
    function spinUntil(profiler, isExpected) {
        do {
            failIfOutOfTime(() => isExpected);
            spin(1);
        } while (!isExpected(profiler.releaseSamples()));
    }

    // A thread that is kept from running on its way into report() or out of it collects samples there, so one batch proves nothing wrong.
    for (let own, witnessed; !(witnessed > 4 * own);) {
        const previous = `${own} of ${witnessed}`;
        const [reporter, witness] = [create(), create()];
        spinUntil(reporter, hasAny);
        for (own = witnessed = 0; witnessed < 20;) {
            failIfOutOfTime(() => previous);
            for (let i = 0; i < 10; i++)
                reporter.report();
            own += reporter.releaseSamples().stackTraces.filter(isInReport).length;
            witnessed += witness.releaseSamples().stackTraces.filter(isInReport).length;
        }
        spinUntil(reporter, ({ stackTraces }) => stackTraces.some(({ frames }) => frames.includes("spinUntil")));
        reporter.shutdown();
        witness.shutdown();
    }

    // Before the first sample.
    for (let i = 0; i < 5; i++) {
        const paused = create();
        paused.pause();
        paused.releaseSamples();
        paused.report();
        const running = create(1000 * 1000 * 1000);
        for (let i = 0; i < 10; i++)
            running.report();
        spinUntil(running, hasAny);
        assert(!hasAny(paused.releaseSamples()), "the report has started it");
        paused.shutdown();
        running.shutdown();
    }

    runAsyncTest(async () => {
        const [reporter, witness] = [create(), create()];
        for (let witnessed = 0, rounds = 0; witnessed < 10 || rounds < 10; rounds++) {
            failIfOutOfTime(() => witnessed);
            await new Promise(resolve => {
                for (let i = 0; i <= rounds % 4; i++)
                    setTimeout(reporter.report, stretchInSlowBuild(1));
                setTimeout(() => setTimeout(resolve, stretchInSlowBuild(1)), stretchInSlowBuild(1));
            });
            const runs = reporter.releaseSamples().withoutFrames;
            const { stackTraces, withoutFrames } = witness.releaseSamples();
            for (const { timestamp } of stackTraces.filter(isInReport)) {
                witnessed++;
                assert(!runs.some(run => run.first < timestamp && timestamp < run.last), "a run goes on across a tick that was left out");
            }
            for (const { state, first, last } of withoutFrames)
                assert(state !== "Program" || runs.filter(run => first <= run.last && run.first <= last).length <= 1, "a run was cut in two");
        }
        reporter.shutdown();
        witness.shutdown();
    });
}
