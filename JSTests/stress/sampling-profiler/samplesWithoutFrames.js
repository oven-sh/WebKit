function assert(condition, message) {
    if (!condition)
        throw new Error(message);
}

// The sampler is a thread like any other, so nothing here counts on how often it runs.
function failIfOutOfTime(describe) {
    failIfOutOfTime.startTime ??= Date.now();
    if (Date.now() - failIfOutOfTime.startTime > 50000)
        throw new Error("out of time: " + describe());
}

class Tally {
    Idle = 0;
    Program = 0;
    GarbageCollector = 0;
    JavaScript = 0;
    stackTraces = 0;
    longestRun = 0;
    topFrames = new Map;

    constructor() {
        releaseSamplingProfilerSamples();
    }

    get withoutFrames() { return this.Idle + this.Program + this.GarbageCollector + this.JavaScript; }
    get total() { return this.withoutFrames + this.stackTraces; }
    withTopFrame(name) { return this.topFrames.get(name) ?? 0; }
    toString() { return JSON.stringify({ ...this, topFrames: [...this.topFrames] }); }

    update() {
        const { stackTraces, withoutFrames } = releaseSamplingProfilerSamples();

        this.stackTraces += stackTraces.length;
        for (const { frames } of stackTraces)
            this.topFrames.set(frames[0], this.withTopFrame(frames[0]) + 1);

        let nextStackTrace = 0;
        let previous;
        for (const run of withoutFrames) {
            this[run.state] += run.count;
            this.longestRun = Math.max(this.longestRun, run.count);
            assert(run.count === 1 ? run.first === run.last : run.first < run.last, "bad run " + JSON.stringify(run));
            assert(!previous || previous.last <= run.first, "runs overlap");

            const stackTracesBefore = nextStackTrace;
            while (stackTraces[nextStackTrace]?.timestamp <= run.first)
                nextStackTrace++;
            assert(!(stackTraces[nextStackTrace]?.timestamp < run.last), "a stack trace was taken within a run");
            assert(previous?.state !== run.state || nextStackTrace !== stackTracesBefore, "a run was cut in two");
            previous = run;
        }
        return stackTraces;
    }

    repeatUntil(isDone, step) {
        while (!isDone(this)) {
            failIfOutOfTime(() => this);
            step();
            this.update();
        }
        return this;
    }

    async repeatAsyncUntil(isDone, step) {
        while (!isDone(this)) {
            failIfOutOfTime(() => this);
            await step();
            this.update();
        }
        return this;
    }
}

// A step for repeatUntil(), of which only run() is sampled.
function sampleOnly(run, prepare) {
    return () => {
        pauseSamplingProfiler();
        prepare();
        releaseSamplingProfilerSamples();
        startSamplingProfiler();
        run();
        pauseSamplingProfiler();
    };
}

// A thread that is kept from running collects samples wherever it happens to be, so one batch proves nothing wrong.
function expectCollectorRatherThan(name, step) {
    for (let tally; !(tally?.GarbageCollector > 4 * tally?.withTopFrame(name));) {
        failIfOutOfTime(() => `${name}: ${tally}`);
        tally = new Tally().repeatUntil(tally => tally.GarbageCollector + tally.withTopFrame(name) >= 100, step);
    }
}

function spin(milliseconds) {
    const end = preciseTime() + milliseconds / 1000;
    while (preciseTime() < end) { }
}
noInline(spin);

function runAsyncTest(test) {
    asyncTestStart(1);
    test().then(asyncTestPassed, error => {
        print(error, "\n", error.stack);
        $vm.abort();
    });
}
