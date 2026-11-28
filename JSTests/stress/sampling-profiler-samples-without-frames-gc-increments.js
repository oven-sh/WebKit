//@ runDefault("--gcIncrementScale=1000", "--gcIncrementMaxBytes=100000000", "--numberOfGCMarkers=1")

// No other thread marks, so it is left to whoever allocates.
if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    const live = [];
    for (let i = 0; i < 200000; i++)
        live.push({ i });

    function allocate() {
        for (let i = 0; i < 2000; i++)
            live[0] = [i, i];
    }
    noInline(allocate);

    startSamplingProfiler({ intervalInMicroseconds: 200, recordsSamplesWithoutFrames: true });
    expectCollectorRatherThan("allocate", sampleOnly(allocate, $vm.gcSweepAsynchronously));
}
