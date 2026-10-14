if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplingProfiler.js", "caller relative");

    function inner(x) {
        let sum = 0;
        for (let i = 0; i < 100; i++)
            sum += (x ^ i) * 3;
        return sum;
    }

    function outer(count) {
        let total = 0;
        for (let i = 0; i < count; i++)
            total += inner(i);
        return total;
    }
    noInline(outer);

    // Nothing counts until outer() has had the time to reach the top tier, where inner() is inlined
    // and only the PC tells the two apart.
    for (let i = 0; i < testLoopCount; i++)
        outer(10);
    samplingProfilerStackTraces();

    const lines = { inner: [4, 9], outer: [11, 16] };
    const samples = { inner: 0, outer: 0 };
    while (samples.inner + samples.outer < 20) {
        outer(1000);
        for (const trace of samplingProfilerStackTraces().traces) {
            const top = trace.frames[0];
            if (!(top.name in samples))
                continue;
            samples[top.name]++;
            if (!(top.line >= lines[top.name][0] && top.line <= lines[top.name][1]))
                throw new Error(`${top.name}() is not at line ${top.line}: ${JSON.stringify(trace)}`);
        }
    }

    // Almost all the time is spent in the loop of inner().
    if (samples.inner <= samples.outer)
        throw new Error(`inner() has ${samples.inner} samples and outer() has ${samples.outer}`);
}
