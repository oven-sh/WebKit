// Bun: upstream runs this in every mode. A frame of FTL code is sampled without a line, so a sample has to fall into the few milliseconds before test() gets there.
//@ $skipModes << "ftl-eager".to_sym
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}
(function () {
    if (!platformSupportsSamplingProfiler())
        return;

    load("./sampling-profiler/samplingProfiler.js", "caller relative");

    function test() {
        var res = 0;
        for (let i = 0; i < 1000; ++i)
            res += Math.tan(i);
        return res;
    }
    noInline(test);

    const timeToFail = 50000;
    let startTime = Date.now();
    do {
        test();
        let data = samplingProfilerStackTraces();
        for (let trace of data.traces) {
            for (let frame of trace.frames) {
                if (frame.name === 'test' && (frame.line >= 11 && frame.line <= 16) && (frame.column >= 5 && frame.line <= 38))
                    return;
            }
        }
    } while (Date.now() - startTime < timeToFail);
    let stacktraces = samplingProfilerStackTraces();
    throw new Error(`Bad stack trace ${JSON.stringify(stacktraces)}`);
}());
