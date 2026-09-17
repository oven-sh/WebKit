//@ requireOptions("--useJSThreads=1")
// SPEC-ungil history, tenth round, "The profiler's lock and park sites". The
// report entry points hold the sampling profiler's lock while they name the
// frames, and naming a function looks a property up, which polls traps: GIL off a
// reader parked there, for another reader's stop, with the lock held - and that
// stop's conductor needed the lock to process its frames. Found by the mirror
// harness (two threads running sampling-profiler-stack-trace-with-double-quote-
// in-function-name.js at once hung until the deadline). GIL off the report entry
// points now run as the conductor of a stop. Here three threads read the traces
// in a loop while they and a fourth run hot functions with display names, and a
// collection is requested now and then (its profiler constraint takes the same
// lock with the world stopped). The test passes by finishing.
load("../harness.js", "caller relative");

if (platformSupportsSamplingProfiler()) {
    startSamplingProfiler();

    function hot(n) {
        let obj = {};
        for (let i = 0; i < n; ++i)
            obj["p" + (i & 7)] = i;
        return obj;
    }
    noInline(hot);
    hot.displayName = "a \"quoted\" name";

    const gate = { stop: 0 };
    const busy = new Thread(() => {
        let n = 0;
        while (!Atomics.load(gate, "stop")) {
            hot(200);
            if (!(++n & 255))
                gc();
            if (!(n & 15))
                Atomics.wait(gate, "stop", 0, 1); // a blocking call: with the GIL on this is where the readers get to run
        }
        return n;
    });

    function reader(rounds) {
        let frames = 0;
        for (let r = 0; r < rounds; ++r) {
            hot(100);
            if (!(r & 7))
                sleepMs(1);
            const traces = samplingProfilerStackTraces();
            for (const trace of traces.traces)
                frames += trace.frames.length;
        }
        return frames;
    }

    const readers = [new Thread(reader, 300), new Thread(reader, 300)];
    reader(300);
    for (const thread of readers)
        thread.join();
    Atomics.store(gate, "stop", 1);
    busy.join();
}
