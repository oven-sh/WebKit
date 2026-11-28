//@ defaultRun
//@ runDefault("--samplingProfilerMaxStackDepth=50")
//@ runDefault("--samplingProfilerMaxStackDepth=320")

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    function recurse(depth) {
        if (!depth)
            return spin(1);
        recurse(depth - 1);
        return depth;
    }
    noInline(recurse);

    function waitForStackTrace(isExpected, check = () => { }) {
        const tally = new Tally;
        for (;;) {
            failIfOutOfTime(() => tally);
            startSamplingProfiler();
            recurse(350);
            pauseSamplingProfiler();
            const stackTraces = tally.update();
            stackTraces.forEach(check);
            if (stackTraces.some(isExpected))
                return;
        }
    }

    const isInSpin = ({ frames }) => frames.slice(0, 2).includes("spin");
    const countRecurse = ({ frames }) => frames.filter(name => name === "recurse").length;

    // Not set, below what there is room for by then, above it, below what it has grown to, and no longer set.
    for (const maximumStackDepth of [undefined, 100, 300, 2, Infinity]) {
        const expected = jscOptions().samplingProfilerMaxStackDepth || (maximumStackDepth ?? Infinity);
        startSamplingProfiler({ intervalInMicroseconds: 500, maximumStackDepth });
        waitForStackTrace(
            stackTrace => expected === Infinity ? countRecurse(stackTrace) === 351 : isInSpin(stackTrace) && stackTrace.frames.length === expected,
            stackTrace => assert(countRecurse(stackTrace) <= expected, `${stackTrace.frames.length} frames, ${expected} at most`));
    }
}
