if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplingProfiler.js", "caller relative");

    // Long enough for the call to cost next to nothing beside it, whichever tiers the two functions are in.
    function inner(x) {
        let sum = 0;
        for (let i = 0; i < 1000; i++)
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

    // In the top tiers inner() is inlined, and only the PC tells the two apart. What is sampled on the way there does not count.
    for (let i = 0; i < testLoopCount; i++)
        outer(2);
    samplingProfilerStackTraces();

    const timeToFail = 50000;
    const startTime = Date.now();
    let innerSamples = 0;
    let outerSamples = 0;
    while (innerSamples + outerSamples < 40) {
        if (Date.now() - startTime > timeToFail)
            throw new Error(`after ${timeToFail} ms inner() has ${innerSamples} samples and outer() has ${outerSamples}`);
        outer(100);
        for (const trace of samplingProfilerStackTraces().traces) {
            const name = trace.frames[0].name;
            if (name === "inner")
                innerSamples++;
            else if (name === "outer")
                outerSamples++;
        }
    }

    if (innerSamples <= outerSamples)
        throw new Error(`inner() has ${innerSamples} samples and outer() has ${outerSamples}`);
}
