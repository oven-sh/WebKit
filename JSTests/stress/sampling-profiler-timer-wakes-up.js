if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    const waitForSamples = (tally, count) => tally.repeatUntil(tally => tally.total >= count, () => spin(1));

    function startLongWait() {
        startSamplingProfiler({ intervalInMicroseconds: 1000 });
        waitForSamples(new Tally, 1);
        pauseSamplingProfiler();
        const tally = new Tally;
        startSamplingProfiler({ intervalInMicroseconds: 1000 * 1000 * 1000 });
        waitForSamples(tally, 1);
    }

    startSamplingProfiler({ recordsSamplesWithoutFrames: true });

    startLongWait();
    startSamplingProfiler({ intervalInMicroseconds: 1000 });
    waitForSamples(new Tally, 5);

    startLongWait();
    pauseSamplingProfiler();
    let tally = new Tally;
    startSamplingProfiler();
    waitForSamples(tally, 1);

    // Nothing changes, so there is nothing to wake up for.
    tally = new Tally;
    for (let i = 0; i < 20; i++) {
        startSamplingProfiler({ intervalInMicroseconds: 1000 * 1000 * 1000 });
        spin(1);
    }
    tally.update();
    assert(!tally.total, "sampled ahead of time: " + tally);

    pauseSamplingProfiler();
    startSamplingProfiler({ intervalInMicroseconds: 1000 });
    waitForSamples(new Tally, 5);
    pauseSamplingProfiler();
    tally = new Tally;
    spin(10);
    tally.update();
    assert(!tally.total, "sampled while paused: " + tally);
}
