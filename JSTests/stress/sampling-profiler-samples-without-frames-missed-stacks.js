//@ requireOptions("--forceSamplingProfilerToMissStacks=1", "--useSamplingProfiler=1")
//@ defaultNoEagerRun

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    function work() {
        spin(0.2);
        // These enter the VM again, in one way and in the other.
        JSON.stringify({ toJSON: () => spin(0.2) });
        new Float64Array(2).sort(function compare() { spin(0.2); });
    }
    noInline(work);

    const entries = {
        noArguments: resolve => Promise.resolve().finally(function noArguments() { work(); resolve(); }),
        oneArgument: resolve => Promise.resolve(1).then(function oneArgument() { work(); resolve(); }),
        timer: resolve => setTimeout(function timer() { work(); resolve(); }, 0),
        thenable: resolve => Promise.resolve().then(() => ({ then: function thenable() { work(); resolve(); } })),
        asyncFunction: resolve => (async function asyncFunction() { await undefined; work(); resolve(); })(),
        asyncGenerator: resolve => (async function* asyncGenerator() { await undefined; work(); resolve(); })().next(),
        sleepSeconds: resolve => Promise.resolve(0.001).then(sleepSeconds).then(resolve),
        "bound work": resolve => Promise.resolve().then(work.bind()).then(resolve),
    };

    startSamplingProfiler({ intervalInMicroseconds: 100 });
    let tally = new Tally;
    work();
    tally.update();
    assert(!tally.total, "they have to be asked for: " + tally);

    startSamplingProfiler({ recordsSamplesWithoutFrames: true });
    pauseSamplingProfiler();
    function expectOnly(name) {
        const tally = new Tally().repeatUntil(tally => tally.stackTraces >= 10, sampleOnly(work));
        assert(tally.withStack(name) === tally.stackTraces && !tally.Program, `${name}: ${tally}`);
    }
    expectOnly("(program)");

    runAsyncTest(async () => {
        globalThis.runInModule = () => expectOnly("(module)");
        await import("./sampling-profiler/module.js");

        for (const [name, enter] of Object.entries(entries)) {
            const tally = await new Tally().repeatAsyncUntil(tally => tally.withStack(name) >= 10, () => {
                startSamplingProfiler();
                return new Promise(enter).then(pauseSamplingProfiler);
            });
            const others = Object.keys(entries).filter(other => other !== name).concat("work", "spin", "toJSON", "compare", "(no frames)");
            assert(!others.some(other => tally.withStack(other)), `${name}: ${tally}`);
        }
    });
}
