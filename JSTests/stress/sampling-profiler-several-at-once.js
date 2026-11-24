//@ defaultNoEagerRun
//@ runDefault("--destroy-vm")

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    const never = 1000 * 1000 * 1000;
    const create = options => {
        const profiler = createSamplingProfiler({ intervalInMicroseconds: 500, ...options });
        return Object.assign(new Tally(profiler.releaseSamples), { shutdown: profiler.shutdown });
    };
    const sample = (tally, count = 10, start = tally.total) => tally.repeatUntil(tally => tally.total >= start + count, () => spin(1));
    const update = tally => (tally.update(), tally);

    runAsyncTest(async () => {
        setSamplingProfilerThreadIsIdle(true);
        const records = create({ recordsSamplesWithoutFrames: true });
        const doesNotRecord = create();
        await records.repeatAsyncUntil(tally => tally.Idle >= 5, () => new Promise(resolve => setTimeout(resolve, 2)));
        assert(!update(doesNotRecord).withoutFrames, "it did not ask for them: " + doesNotRecord);
        records.shutdown();
        doesNotRecord.shutdown();

        const slow = create({ intervalInMicroseconds: never });
        const shallow = create({ maximumStackDepth: 1 });
        const deep = create();
        sample(shallow);
        deep.repeatUntil(tally => tally.withStackIncluding("spin", ""), () => spin(1));
        assert(update(slow).total <= 1, "it has the interval of another: " + slow);
        assert(!update(shallow).withStackIncluding("runAsyncTest"), "it has the depth of another: " + shallow);

        // What one releases, the other still has.
        function marker() { spin(1); }
        noInline(marker);
        for (let other; !other || !update(other).withStackIncluding("marker");) {
            other?.shutdown();
            other = create();
            const before = deep.total;
            deep.repeatUntil(tally => tally.total >= before + 10, marker);
            sample(deep, 1);
        }

        shallow.shutdown();
        shallow.shutdown();
        const total = shallow.total;
        sample(deep);
        assert(update(shallow).total === total, "sampled after its shutdown: " + shallow);

        // Nor does the one that belongs to the VM have a say.
        startSamplingProfiler({ intervalInMicroseconds: never });
        pauseSamplingProfiler();
        sample(deep);
        deep.shutdown();

        // The collector keeps what the samples refer to.
        for (let found = 0; found < 20;) {
            failIfOutOfTime(() => found);
            const profilers = [create(), create()];
            for (let i = 0; i < 10; i++)
                new Function("spin", `return function temporary${i}() { spin(0.5); }`)(spin)();
            fullGC();
            for (let i = 0; i < 100; i++)
                new Function(`return function other${i}() { }`)()();
            fullGC();
            for (const profiler of profilers) {
                for (const stack of update(profiler).stacks.keys()) {
                    const frames = stack.split(" < ");
                    if (!frames.includes("spin"))
                        continue;
                    assert(frames[frames.indexOf("spin") + 1].startsWith("temporary"), "bad stack " + stack);
                    found++;
                }
                profiler.shutdown();
            }
        }

        // The VM shuts down what is left.
        create();
        for (let i = 0; i < 10; i++)
            $.agent.start(`createSamplingProfiler({ intervalInMicroseconds: 10, recordsSamplesWithoutFrames: true }); $.agent.report(""); $.agent.leaving();`);
        for (let reports = 0; reports < 10;)
            reports += $.agent.getReport() !== null;
        for (const end = preciseTime() + 0.02; preciseTime() < end;)
            new ArrayBuffer(100000); // In the place of what those have freed.
    });
}
