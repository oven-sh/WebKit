//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-ungil §A.1.7 form (i), AUD1.K1 / SD18 (history, tenth round). The sampling
// profiler suspends the thread that runs JS and reads its entry record, top
// frames and executing RegExp. GIL off those live in the thread's VMLite, and
// until the tenth round a GIL-off VM simply never sampled (a logged refusal):
// every test of the profiler failed GIL off and a program that profiled itself
// got empty traces. Now the carrier that binds as the sampled thread records its
// lite and the profiler thread reads it through the registry. Spawned threads
// stay unsampled by ruling (SD18).
// Checked, in both modes: the main thread's hot function shows up in the traces
// while a second thread runs its own hot function; GIL off the second thread's
// function never does (GIL on either may: the sampled thread is whoever holds
// the GIL). Reading the traces while the other thread allocates exercises the
// stop that turning raw frames into traces needs on a shared heap.
load("../harness.js", "caller relative");

if (platformSupportsSamplingProfiler()) {
    startSamplingProfiler();

    function carrierHot(n) {
        let s = 0;
        for (let i = 0; i < n; ++i)
            s += (i * 7) % 11;
        return s;
    }
    noInline(carrierHot);

    const gate = { stop: 0, started: 0 };
    const other = new Thread(() => {
        function spawnedHot(n) {
            let s = 0;
            const keep = [];
            for (let i = 0; i < n; ++i) {
                s += (i * 5) % 13;
                if (!(i & 1023))
                    keep.push({ i }); // allocates: the reader's stop has somebody to park
            }
            return s + keep.length;
        }
        noInline(spawnedHot);
        Atomics.store(gate, "started", 1);
        let total = 0;
        while (!Atomics.load(gate, "stop")) {
            total += spawnedHot(200000);
            Atomics.wait(gate, "stop", 0, 1); // a blocking call: with the GIL on this is where the main thread gets to run
        }
        return total;
    });
    waitUntil(() => Atomics.load(gate, "started") === 1);

    function namesIn(traces) {
        const names = new Set();
        for (const trace of traces.traces) {
            for (const frame of trace.frames)
                names.add(frame.name);
        }
        return names;
    }

    const seen = new Set();
    const deadline = preciseTime() + 20;
    let reads = 0;
    while (!seen.has("carrierHot") && preciseTime() < deadline) {
        for (let i = 0; i < 40; ++i)
            carrierHot(200000);
        for (const name of namesIn(samplingProfilerStackTraces()))
            seen.add(name);
        ++reads;
        sleepMs(1); // and this is where the other thread does, with the GIL on
    }
    // A few more reads with the other thread still running.
    for (let r = 0; r < 5; ++r) {
        for (let i = 0; i < 20; ++i)
            carrierHot(200000);
        for (const name of namesIn(samplingProfilerStackTraces()))
            seen.add(name);
        sleepMs(1);
    }
    Atomics.store(gate, "stop", 1);
    other.join();

    if (!seen.has("carrierHot"))
        throw new Error("the main thread's hot function was never sampled in " + reads + " reads; saw: " + [...seen].join(", "));
    if (!$vm.useThreadGIL() && seen.has("spawnedHot"))
        throw new Error("a spawned thread's frame was sampled GIL off (SD18 rules them out)");
}

