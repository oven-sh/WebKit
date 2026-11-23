if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    const leaveVM = () => new Promise(resolve => setTimeout(resolve, 2));

    runAsyncTest(async () => {
        let intervalInMicroseconds = 200;
        startSamplingProfiler({ intervalInMicroseconds });
        let tally = await new Tally().repeatAsyncUntil(tally => tally.stackTraces >= 10, () => (spin(1), leaveVM()));
        assert(!tally.withoutFrames, "they have to be asked for: " + tally);

        startSamplingProfiler({ recordsSamplesWithoutFrames: true });
        tally = await new Tally().repeatAsyncUntil(tally => tally.Program >= 10, leaveVM);
        assert(!tally.Idle, "only the embedder knows that the thread is idle: " + tally);

        setSamplingProfilerThreadIsIdle(true);
        tally = await new Tally().repeatAsyncUntil(tally => tally.Idle >= 10 && tally.longestRun >= 2, leaveVM);
        assert(!tally.Program, "the thread is idle: " + tally);

        tally = new Tally().repeatUntil(tally => tally.withTopFrame("sleepSeconds") >= 10, () => sleepSeconds(0.002));
        assert(!tally.Idle && !tally.Program, "a wait beneath a frame belongs to that frame: " + tally);

        // A stack trace between two waits ends the run of the first.
        await new Tally().repeatAsyncUntil(tally => tally.stackTraces >= 20 && tally.Idle >= 20, async () => {
            await leaveVM();
            spin(1);
            await leaveVM();
        });

        // Whoever takes the stack traces takes the rest of that time as well.
        await new Tally().repeatAsyncUntil(tally => tally.Idle, leaveVM);
        await leaveVM();
        pauseSamplingProfiler();
        samplingProfilerStackTraces();
        assert(!releaseSamplingProfilerSamples().withoutFrames.length, "they outlived the stack traces");

        // The samples of a run are about evenly spaced.
        const interruptions = {
            async pause() {
                pauseSamplingProfiler();
                const time = $.agent.monotonicNow();
                await leaveVM();
                startSamplingProfiler();
                return time;
            },
            changeInterval() {
                startSamplingProfiler({ intervalInMicroseconds: ++intervalInMicroseconds });
                return $.agent.monotonicNow();
            },
            async stopRecording() {
                startSamplingProfiler({ recordsSamplesWithoutFrames: false });
                const time = $.agent.monotonicNow();
                await leaveVM();
                startSamplingProfiler({ recordsSamplesWithoutFrames: true });
                return time;
            },
            changeState() {
                startSamplingProfiler(); // Waits for a sample that has its timestamp already.
                setSamplingProfilerThreadIsIdle(false);
                return $.agent.monotonicNow();
            },
        };
        for (const [name, interrupt] of Object.entries(interruptions)) {
            for (let count = 0; count < 4;) {
                failIfOutOfTime(() => name);
                setSamplingProfilerThreadIsIdle(true);
                startSamplingProfiler();
                await leaveVM();
                const before = $.agent.monotonicNow();
                const after = await interrupt();
                await leaveVM();
                pauseSamplingProfiler();
                const runs = releaseSamplingProfilerSamples().withoutFrames;
                assert(!runs.some(run => run.first < before && after < run.last), "a run goes on after " + name);
                count += runs.some(run => run.last < before) && runs.some(run => after < run.first);
            }
        }

        // The timer never spins.
        for (let count = 0; count < 50;) {
            failIfOutOfTime(() => count);
            startSamplingProfiler({ intervalInMicroseconds: 0 });
            await leaveVM();
            pauseSamplingProfiler();
            for (const run of releaseSamplingProfilerSamples().withoutFrames) {
                count += run.count;
                assert((run.count - 1) * 0.009 <= run.last - run.first, "less than 10 microseconds apart: " + JSON.stringify(run));
            }
        }

        startSamplingProfiler({ intervalInMicroseconds, recordsSamplesWithoutFrames: false });
        tally = await new Tally().repeatAsyncUntil(tally => tally.stackTraces >= 10, () => (spin(1), leaveVM()));
        assert(!tally.withoutFrames, "they are no longer asked for: " + tally);
    });
}
