//@ requireOptions("--useJSThreads=1")
// Tenth round (AUDIT R10-30). The sampling profiler suspends the thread bound as
// "the thread that runs JS", and with the GIL on that binding follows the GIL:
// every spawned Thread binds when it takes the lock. A spawned thread that was
// the last to take it and then exited stayed bound, and the profiler's next
// sample signalled a thread that no longer exists and waited for its answer
// forever - holding the machine-threads lock the main thread needs to take the
// GIL back. Found by the amplifier on the profiler's new test (2 hangs in 500
// GIL-on runs; 1 in 1,000 on the tree before the GIL-off profiler change).
// Here the main thread sleeps (which gives the GIL away) while spawned threads
// run and exit, so for most of each sleep the last bound thread is gone and the
// 1 ms sampling timer fires many times. An exiting Thread now unbinds itself.
// GIL off spawned threads never bind; the test runs there too.
load("../harness.js", "caller relative");

if (platformSupportsSamplingProfiler()) {
    startSamplingProfiler();

    function work(n) {
        let s = 0;
        for (let i = 0; i < n; ++i)
            s += i % 7;
        return s;
    }
    noInline(work);

    for (let round = 0; round < 12; ++round) {
        const t = new Thread(() => work(20000));
        sleepMs(15); // the thread runs, returns and exits while this thread is not holding the GIL
        work(20000);
        shouldBe(t.join(), work(20000));
    }
    // The traces are still readable.
    shouldBeTrue(Array.isArray(samplingProfilerStackTraces().traces));
}
