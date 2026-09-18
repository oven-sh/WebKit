// Bun: upstream has no requireOptions here. A sample of test() has a line only while test() is LLInt or baseline code, or
// when the code it runs has a PC to CodeOrigin map, and the shell's startSamplingProfiler() does not turn that map on.
// In the eager modes test() is FTL code before its first call returns, and the wait below times out.
//@ requireOptions("--alwaysGeneratePCToCodeOriginMap=true")
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
