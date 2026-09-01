//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--useSamplingProfiler=1")
// GIL-off the sampling profiler refuses to sample: a thread's entry scope, top
// call frame and executing RegExp live in its VMLite, which the profiler thread
// cannot resolve, so the VM logs the refusal once at construction and never
// starts the sampling thread. The profiler object still exists, so the shell
// API keeps working and returns the defined empty result. This drives that
// path through VM entries on the main thread and on spawned threads (the
// per-thread entry-record assert in noticeVMEntry), a RegExp match, a
// startSamplingProfiler() call on the main thread and on a spawned thread
// (which never becomes the sampled thread), and the traces API.
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
        let y = 0;
        for (let i = 0; i < 100000; i++)
            y += (i * (t + 1)) & 3;
        startSamplingProfiler();
        return y;
    }));
    shouldBe(results.length, 4);
    for (const y of results)
        shouldBeTrue(Number.isInteger(y) && y >= 0, "spawned thread result " + describe(y));

    startSamplingProfiler();
    shouldBe(busy(100000), 299995);

    const traces = samplingProfilerStackTraces();
    shouldBeTrue(Array.isArray(traces.traces), "samplingProfilerStackTraces().traces is an array");
    shouldBe(traces.traces.length, 0, "a GIL-off VM takes no samples");
}
