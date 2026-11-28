//@ runDefault
//@ runDefault("--useConcurrentGC=0")
//@ runMiniMode

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    startSamplingProfiler({ intervalInMicroseconds: 200 });
    new Tally().repeatUntil(tally => tally.withTopFrame("fullGC") >= 20, fullGC);

    startSamplingProfiler({ recordsSamplesWithoutFrames: true });

    // Next to nothing to mark, and many blocks to free.
    expectCollectorRatherThan("gc", sampleOnly(gc, () => {
        globalThis.held = [];
        for (let i = 0; i < 10000; i++)
            held.push(new Array(500));
        held = null;
    }));

    // Marking a list takes long, so the collector's thread gets to do most of it.
    for (let i = 0; i < 100000; i++)
        held = { next: held };
    startSamplingProfiler();
    expectCollectorRatherThan("fullGC", fullGC);
}
