//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--useSamplingProfiler=1")
// GIL off the sampling profiler samples the carrier thread through its VMLite
// (SPEC-ungil §A.1.7 form (i); until the tenth round it refused to sample at
// all and this test asserted empty traces) and never a spawned thread (SD18).
// This drives the per-thread paths: VM entries on the main thread and on
// spawned threads (the per-thread entry-record assert in noticeVMEntry), a
// RegExp match, a startSamplingProfiler() call on the main thread and on
// spawned threads (which never become the sampled thread), and the traces API,
// whose heap iteration stops the other threads.
load("../harness.js", "caller relative");

if (typeof platformSupportsSamplingProfiler === "function" && platformSupportsSamplingProfiler()) {
    function busy(n) {
        let x = 0;
        for (let i = 0; i < n; i++)
            x += i % 7;
        return x;
    }
    noInline(busy);

    shouldBe(busy(100000), 299995);
    shouldBeTrue(/a+b/.test("xaaab"));

    const results = joinAll(spawnN(4, t => {
        function onlyOnSpawnedThreads(n) {
            let y = 0;
            for (let i = 0; i < n; i++)
                y += (i * (t + 1)) & 3;
            return y;
        }
        noInline(onlyOnSpawnedThreads);
        startSamplingProfiler();
        let y = 0;
        for (let k = 0; k < 40; ++k)
            y += onlyOnSpawnedThreads(100000);
        return y;
    }));
    shouldBe(results.length, 4);
    for (const y of results)
        shouldBeTrue(Number.isInteger(y) && y >= 0, "spawned thread result " + describe(y));

    startSamplingProfiler();
    shouldBe(busy(100000), 299995);

    const traces = samplingProfilerStackTraces();
    shouldBeTrue(Array.isArray(traces.traces), "samplingProfilerStackTraces().traces is an array");
    for (const trace of traces.traces) {
        for (const frame of trace.frames)
            shouldBeTrue(frame.name !== "onlyOnSpawnedThreads", "a spawned thread's frame was sampled");
    }
}
