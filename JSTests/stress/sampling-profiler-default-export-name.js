if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplingProfiler.js", "caller relative");

    // The DFG and the FTL inline the default export into caller(). A frame of an inlined function has no function object,
    // so the profiler names it from its executable. An anonymous default export is named "default" there too.
    let inlined;
    import("./resources/sampling-profiler-default-export-name.js").then((module) => {
        inlined = module.default;
    });
    drainMicrotasks();

    function work() {
        let sum = 0;
        for (let i = 0; i < 1000; ++i)
            sum += i % 7;
        return sum;
    }
    noInline(work);

    function caller() {
        return inlined(work) + 1;
    }
    noInline(caller);

    // Where the DFG cannot compile caller(), the frame has a function object and the name is read from it.
    for (let i = 0; i < testLoopCount && !numberOfDFGCompiles(caller); ++i)
        caller();
    samplingProfilerStackTraces();

    // A round has a few samples in work(). The fifth sample with the frame between work() and caller() ends the test.
    let found = 0;
    for (let round = 0; round < 100 && found < 5; ++round) {
        for (let i = 0; i < 1000; ++i)
            caller();
        for (const trace of samplingProfilerStackTraces().traces) {
            const index = trace.frames.findIndex((frame) => frame.name === "work");
            if (index < 0 || !trace.frames[index + 2] || trace.frames[index + 2].name !== "caller")
                continue;
            if (trace.frames[index + 1].name !== "default")
                throw new Error(`bad frame name: ${trace.frames.map((frame) => `${frame.name} (${frame.category})`).join(" < ")}`);
            ++found;
        }
    }
    if (!found)
        throw new Error("no sample of the default export");
}
