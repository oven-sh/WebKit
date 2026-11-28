//@ defaultNoEagerRun
//@ runDefault("--forceSamplingProfilerToMissStacks=1", "--useSamplingProfiler=1")
//@ runDefault("--scribbleFreeCells=1", "--sweepSynchronously=1")
//@ runDefault("--forceSamplingProfilerToFailAtStackDepth=400", "--useSamplingProfiler=1")

if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    pauseSamplingProfiler();

    class Profiler {
        stackTraces = [];
        total = 0;
        #profiler;

        constructor(intervalInMilliseconds, options) {
            this.#profiler = createSamplingProfiler({ intervalInMicroseconds: intervalInMilliseconds * 1000, recordsSamplesWithoutFrames: true, ...options });
        }

        update() {
            const { stackTraces, withoutFrames } = this.#profiler.releaseSamples();
            this.stackTraces.push(...stackTraces);
            this.total += withoutFrames.reduce((sum, { count }) => sum + count, stackTraces.length);
            return this;
        }
        sample(count, step = () => spin(1)) {
            for (const start = this.stackTraces.length; this.update().stackTraces.length < start + count;) {
                failIfOutOfTime(() => this.stackTraces.length);
                step();
            }
            return this;
        }
        waitForTick() {
            for (const start = this.total; this.update().total === start;)
                failIfOutOfTime(() => this.total);
            return this;
        }
        shutdown() {
            this.update();
            this.#profiler.shutdown();
            return this;
        }
        // Where it takes longer to look at samples than to collect them, that must not make more of them.
        static shutdownAll(profilers) {
            profilers.forEach(profiler => profiler.#profiler.pause());
            profilers.forEach(profiler => profiler.shutdown());
        }
        static sampleOnly(step, profilers, isDone) {
            while (!isDone()) {
                failIfOutOfTime(() => profilers.map(({ total }) => total));
                profilers.forEach(profiler => profiler.#profiler.start());
                step();
                profilers.forEach(profiler => profiler.#profiler.pause());
                profilers.forEach(profiler => profiler.update());
            }
        }

        get timestamps() { return this.stackTraces.map(({ timestamp }) => timestamp); }
        get gaps() { return this.timestamps.slice(1).map((timestamp, i) => timestamp - this.stackTraces[i].timestamp); }
        get shortestGap() { return Math.min(...this.gaps); }
        expectOnlyTicksOf(other) {
            const ticks = new Set(other.timestamps);
            assert(this.timestamps.every(timestamp => ticks.has(timestamp)), `${this.timestamps} are not among ${other.timestamps}`);
        }
    }

    // The same interval, whenever they start.
    {
        const first = new Profiler(1).sample(3);
        const others = [];
        for (let i = 0; i < 20; i++) {
            others.push(new Profiler(1));
            spin(0.3);
        }
        others.at(-1).sample(10);
        Profiler.shutdownAll([...others, first]);
        for (const other of others) {
            other.stackTraces.shift();
            other.expectOnlyTicksOf(first);
        }
        const sinceLast = first.timestamps.filter(timestamp => timestamp >= others.at(-1).timestamps[0] && timestamp <= others[0].timestamps.at(-1));
        for (const other of others)
            assert(sinceLast.every(timestamp => other.timestamps.includes(timestamp)), "missed a tick");

        // Each has a stopwatch of its own.
        const startsOfStopwatch = ({ stackTraces }) => stackTraces.map(({ timestamp, stopwatchTimestamp }) => timestamp - stopwatchTimestamp);
        let previousStart = -Infinity;
        for (const starts of [first, ...others].map(startsOfStopwatch)) {
            assert(Math.min(...starts) > previousStart && Math.max(...starts) - Math.min(...starts) < 0.01, "not the time of its stopwatch: " + starts);
            previousStart = starts[0];
        }
    }

    // But each takes a sample when it starts.
    {
        const profilers = [];
        for (const interval of [1000, 1000, 2000, 1000, 500])
            profilers.push(new Profiler(interval * 1000).waitForTick());
        for (const profiler of profilers)
            assert(profiler.shutdown().total === 1, "sampled ahead of time: " + profiler.total);
    }

    // That does not put off the next tick.
    for (let delay = Infinity; delay > 125;) {
        failIfOutOfTime(() => delay);
        const start = preciseTime();
        const first = new Profiler(100).waitForTick();
        spin(50);
        const second = new Profiler(100);
        first.update().waitForTick();
        delay = first.total === 2 ? (preciseTime() - start) * 1000 : Infinity;
        Profiler.shutdownAll([first, second]);
    }

    // None waits for another that has the same interval when a shorter one stops.
    for (let samplesOfLate = 0; samplesOfLate !== 1;) {
        failIfOutOfTime(() => samplesOfLate);
        const fast = new Profiler(1);
        const early = new Profiler(200).waitForTick();
        spin(100);
        const late = new Profiler(200).waitForTick();
        early.update();
        fast.shutdown();
        early.waitForTick().shutdown();
        samplesOfLate = late.shutdown().total;
    }

    // A longer one takes some of the ticks, and keeps its pace when there are no more of them.
    {
        const fast = new Profiler(1);
        const slow = new Profiler(10).sample(8);
        const ticksOfFast = slow.stackTraces.length;
        fast.shutdown();
        slow.sample(4).shutdown();
        assert(slow.shortestGap >= 0.8 * 10 - 5, "too often: " + slow.timestamps);
        slow.stackTraces.length = ticksOfFast;
        slow.stackTraces.shift();
        slow.expectOnlyTicksOf(fast);
        assert(slow.shortestGap >= 0.8 * 10 - 1, "too often: " + slow.timestamps);
    }

    // Not a multiple of the shorter one.
    for (let ratio = Infinity; ratio > 1.6;) {
        failIfOutOfTime(() => ratio);
        const fast = new Profiler(4);
        const { timestamps } = new Profiler(5.6).sample(40).shutdown();
        assert(timestamps.at(-1) - timestamps[0] > 4.8 * (timestamps.length - 1), "too often: " + timestamps);
        ratio = fast.shutdown().stackTraces.length / timestamps.length;
    }

    // A shorter one does not wait for the tick of a longer one, and those that come and go do not make ticks for it.
    {
        const never = new Profiler(1000 * 1000).waitForTick();
        new Profiler(1).sample(5).shutdown();
        for (const interval of [1, 1000 * 1000, 2000 * 1000]) {
            for (let i = 0; i < 10; i++) {
                const other = new Profiler(interval);
                spin(1);
                other.shutdown();
            }
        }
        assert(never.shutdown().total === 1, "sampled ahead of time: " + never.total);
    }

    if (!jscOptions().forceSamplingProfilerToMissStacks) {
        function recurse(depth) {
            if (!depth)
                return spin(1);
            recurse(depth - 1);
            return depth;
        }
        noInline(recurse);

        // One walk serves all depths, in whichever order they come.
        const depths = [5, 300, 20, 5, Infinity, 300];
        const profilers = depths.map(maximumStackDepth => new Profiler(1, { maximumStackDepth }));
        const countRecurse = ({ frames }) => frames.filter(name => name === "recurse").length;
        const unlimited = profilers[depths.indexOf(Infinity)];
        const isAtBottom = stackTrace => countRecurse(stackTrace) === 351;
        const isTickAtBottom = stackTrace => isAtBottom(stackTrace) && profilers.every(({ timestamps }) => timestamps.includes(stackTrace.timestamp));
        Profiler.sampleOnly(() => recurse(350), profilers, () => unlimited.stackTraces.some(isTickAtBottom));
        Profiler.shutdownAll(profilers);
        const { timestamp, frames } = unlimited.stackTraces.find(isTickAtBottom);
        profilers.forEach((profiler, i) => {
            const stackTrace = profiler.stackTraces.find(other => other.timestamp === timestamp);
            assert(String(stackTrace.frames) === String(frames.slice(0, depths[i])), `${stackTrace.frames.length} frames, expected ${depths[i]}`);
        });

        // The one that stays has as much room as before, and no more is kept.
        {
            const deeper = new Profiler(1);
            const stays = new Profiler(1, { maximumStackDepth: 300 });
            Profiler.sampleOnly(() => recurse(350), [deeper, stays], () => deeper.stackTraces.some(isAtBottom));
            deeper.shutdown();
            stays.stackTraces.length = 0;
            const atBottom = () => stays.stackTraces.filter(({ frames }) => frames.includes("spin"));
            Profiler.sampleOnly(() => recurse(350), [stays], () => atBottom().length);
            assert(atBottom().every(({ frames }) => frames.length === 300), "cut off: " + atBottom().map(({ frames }) => frames.length));
            const another = new Profiler(1);
            Profiler.sampleOnly(() => recurse(350), [another], () => another.stackTraces.some(isAtBottom));
            assert(jscOptions().useSamplingProfiler || another.stackTraces.some(({ frames }) => frames.at(-1) === "recurse"), "never cut off");
            Profiler.shutdownAll([stays, another]);
        }

        // A longer one that has been held up does not make up for it.
        {
            const deep = createSamplingProfiler({ intervalInMicroseconds: 100 });
            for (const end = preciseTime() + 0.04; preciseTime() < end;)
                recurse(200);
            deep.pause();
            const [fast, slow] = [0.2, 1].map(interval => new Profiler(interval, { recordsSamplesWithoutFrames: false }));
            spin(2);
            fullGC();
            spin(3);
            deep.shutdown();
            Profiler.shutdownAll([fast, slow]);
            assert(slow.shortestGap >= 0.8 * 1 - 0.2, "too often: " + slow.timestamps);
        }

        // A frame that it cannot make sense of only matters to those that get as far.
        const invalidFrame = jscOptions().forceSamplingProfilerToFailAtStackDepth;
        if (invalidFrame) {
            const profilers = [invalidFrame - 1, invalidFrame, Infinity].map(maximumStackDepth => new Profiler(1, { maximumStackDepth }));
            const depthsAt = ({ timestamp }) => profilers.map(({ stackTraces }) => stackTraces.filter(other => other.timestamp === timestamp).map(({ frames }) => frames.length)).join(" ");
            Profiler.sampleOnly(() => recurse(500), profilers, () => profilers[0].stackTraces.some(stackTrace => depthsAt(stackTrace) === `${invalidFrame - 1} 1 1`));
            Profiler.shutdownAll(profilers);
        }

        // The report of one does not hold up the others.
        for (let share = 1; share > 0.15;) {
            failIfOutOfTime(() => share);
            startSamplingProfiler({ intervalInMicroseconds: 200 });
            for (const end = preciseTime() + 0.04; preciseTime() < end;)
                recurse(100);
            pauseSamplingProfiler();
            const other = new Profiler(1).sample(1);
            samplingProfilerStackTraces();
            const { gaps } = other.update().sample(1).shutdown();
            share = Math.max(...gaps) / gaps.reduce((sum, gap) => sum + gap);
        }

        // A tick of the collector is a stack trace for one, and a sample without frames for the other.
        for (let found = 0; found < 5;) {
            failIfOutOfTime(() => found);
            const records = createSamplingProfiler({ intervalInMicroseconds: 200, recordsSamplesWithoutFrames: true });
            const doesNotRecord = new Profiler(0.2, { recordsSamplesWithoutFrames: false });
            fullGC();
            const { timestamps } = doesNotRecord.shutdown();
            const { stackTraces, withoutFrames } = records.releaseSamples();
            records.shutdown();
            for (const timestamp of timestamps.slice(1)) {
                const run = withoutFrames.find(({ first, last }) => first <= timestamp && timestamp <= last);
                assert(!run !== !stackTraces.some(stackTrace => stackTrace.timestamp === timestamp), "one or the other");
                found += run?.state === "GarbageCollector";
            }
        }

        // The one that looked at a stack trace first is gone by the time the collector runs.
        for (let found = 0; found < 20;) {
            failIfOutOfTime(() => found);
            const [first, second] = [new Profiler(0.5), new Profiler(0.5)];
            for (let i = 0; i < 10; i++)
                new Function("spin", `return function temporary${i}() { spin(0.5); }`)(spin)();
            first.shutdown();
            fullGC();
            for (let i = 0; i < 100; i++)
                new Function(`return function other${i}() { }`)()();
            fullGC();
            for (const { frames } of second.shutdown().stackTraces) {
                if (!frames.includes("spin"))
                    continue;
                assert(frames[frames.indexOf("spin") + 1].startsWith("temporary"), "bad stack " + frames);
                found++;
            }
        }
    }
}
