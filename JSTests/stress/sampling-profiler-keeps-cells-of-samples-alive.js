//@ defaultNoEagerRun
//@ runDefault("--collectExtraSamplingProfilerData=1", "--useConcurrentJIT=0")
//@ runDefault("--collectExtraSamplingProfilerData=1", "--useConcurrentJIT=0", "--scribbleFreeCells=1", "--sweepSynchronously=1")

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    const { useJIT, useDFGJIT, collectExtraSamplingProfilerData } = jscOptions();
    const optimize = name => {
        const [outer, jettison] = new Function("spin", `
            let constant = 0;
            function inner(milliseconds) { spin(milliseconds); return constant; }
            return [function ${name}(milliseconds) { return inner(milliseconds) + 1; }, () => constant++];`)(spin);
        noInline(outer);
        for (let i = 0; i < 2000; i++)
            outer(0);
        return [outer, jettison];
    };
    function match(pattern) {
        const regExp = new RegExp(pattern);
        regExp.test("");
        for (const end = preciseTime() + 0.001; preciseTime() < end;)
            regExp.test("a".repeat(200));
    }
    noInline(match);

    for (let [i, evals, regExps, inliners] = [0, 0, 0, useJIT && useDFGJIT && collectExtraSamplingProfilerData ? 0 : Infinity]; Math.min(evals, regExps, inliners) < 10; i++) {
        failIfOutOfTime(() => `${evals} ${regExps} ${inliners}`);
        const [outer, jettison] = optimize("outer");
        startSamplingProfiler({ intervalInMicroseconds: 200 });
        (0, eval)(`spin(1); //# sourceURL=eval${i}`);
        match(`(a|${i})*b`);
        outer(1);
        pauseSamplingProfiler();
        jettison();
        for (let j = 0; j < 2; j++) {
            fullGC();
            for (let k = 0; k < 70; k++)
                (0, eval)(`new RegExp("other${i}-${j}-${k}").test("")`);
            optimize("other");
        }
        const { traces, sources } = samplingProfilerStackTraces();
        for (const { frames } of traces) {
            frames.forEach(({ name, sourceID, inliner }, depth) => {
                if (name.startsWith("/")) {
                    assert(name === `/(a|${i})*b/`, "another regular expression: " + name);
                    regExps++;
                } else if (frames[depth + 1]?.name === "eval" && frames[depth - 1]?.name === "spin") {
                    assert(sources.find(source => source.sourceID === sourceID).sourceURL === `eval${i}`, "the source of another eval");
                    evals++;
                } else if (name === "inner" && inliner) {
                    assert(inliner.name === "outer", "inlined into " + inliner.name);
                    inliners++;
                }
            });
        }
    }
}
