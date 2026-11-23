//@ runDefault("--collectContinuously=1", "--collectContinuouslyPeriodMS=2", "--useGenerationalGC=0", "--numberOfGCMarkers=1", "--useStochasticMutatorScheduler=1", "--minimumGCPauseMS=10000")

// waitForReport() gives up heap access. Within 2 ms the collector's thread stops the world for a whole collection.
if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    const requests = new Int32Array(new SharedArrayBuffer(8));
    $.agent.start(`
        $.agent.receiveBroadcast(buffer => {
            const requests = new Int32Array(buffer);
            for (let count = 0; ; count++) {
                while (Atomics.load(requests, 0) === count)
                    Atomics.wait(requests, 0, count);
                if (Atomics.load(requests, 1))
                    break;
                $.agent.sleep(5);
                $.agent.report($.agent.monotonicNow());
            }
            $.agent.leaving();
        });
    `);
    $.agent.broadcast(requests.buffer);

    function request() {
        Atomics.add(requests, 0, 1);
        Atomics.notify(requests, 0);
    }

    startSamplingProfiler({ intervalInMicroseconds: 500, recordsSamplesWithoutFrames: true });

    // It has to hold for some three waits in a row: for how long a thread is kept from running is up to the machine.
    // So is how much there has to be to mark for a collection to outlast a wait.
    globalThis.list = null;
    for (let result, length = 0; ;) {
        failIfOutOfTime(() => JSON.stringify(result));
        for (const newLength = Math.min(2 * length || 50000, 3200000); length < newLength; length++)
            list = { next: list };

        result = { before: { inCall: 0, inCollector: 0 }, after: { inCall: 0, inCollector: 0 } };
        for (let i = 0; i < 3; i++) {
            let startTime, reportTime;
            sampleOnly(() => {
                startTime = $.agent.monotonicNow();
                reportTime = waitForReport();
            }, request)();
            const { stackTraces, withoutFrames } = releaseSamplingProfilerSamples();
            for (const { timestamp, frames } of stackTraces)
                result[timestamp < reportTime ? "before" : "after"].inCall += frames[0] === "waitForReport";
            for (const { state, first, count } of withoutFrames) {
                if (state === "GarbageCollector" && startTime < first)
                    result[first < reportTime ? "before" : "after"].inCollector += count;
            }
        }

        const waitForReportBelongsToCall = result.before.inCall > 4 * result.before.inCollector;
        const waitForAccessBelongsToCollector = result.after.inCollector > 4 * result.after.inCall;
        if (waitForReportBelongsToCall && waitForAccessBelongsToCollector)
            break;
    }

    Atomics.store(requests, 1, 1);
    request();

    // So does leaving the VM.
    runAsyncTest(async () => {
        setSamplingProfilerThreadIsIdle(true);
        for (let inARow = 0; inARow < 3;) {
            failIfOutOfTime(() => "a wait for heap access is not idle");
            const timer = new Promise(resolve => setTimeout(resolve, 5));
            startSamplingProfiler();
            const startTime = $.agent.monotonicNow();
            await timer;
            pauseSamplingProfiler();
            const runs = releaseSamplingProfilerSamples().withoutFrames.filter(run => startTime < run.first);
            const lastIdle = runs.findLastIndex(run => run.state === "Idle");
            const next = lastIdle >= 0 && runs[lastIdle + 1];
            inARow = next?.state === "GarbageCollector" && next.count >= 2 ? inARow + 1 : 0;
        }
    });
}
