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
    stackTraces = 0;
    longestRun = 0;
    topFrames = new Map;
    #stacks = new Map;
    #notYetInStacks = [];
    #releaseSamples;

    constructor(releaseSamples = releaseSamplingProfilerSamples) {
        this.#releaseSamples = releaseSamples;
        releaseSamples();
    }

    get withoutFrames() { return this.Idle + this.Program + this.GarbageCollector; }
    get total() { return this.withoutFrames + this.stackTraces; }
    withTopFrame(name) { return this.topFrames.get(name) ?? 0; }
    // Only on demand: what a test does with its samples is sampled too, and has to take less time than it took to collect them.
    get stacks() {
        for (const { frames } of this.#notYetInStacks.splice(0).flat()) {
            const stack = frames.length ? frames.join(" < ") : "(no frames)";
            this.#stacks.set(stack, (this.#stacks.get(stack) ?? 0) + 1);
        }
        return this.#stacks;
    }
    withStack(...names) { return this.stacks.get(names.join(" < ")) ?? 0; }
    withStackIncluding(...names) { return [...this.stacks].reduce((sum, [stack, count]) => sum + (stack.includes(names.join(" < ")) ? count : 0), 0); }
    toString() { return JSON.stringify({ ...this, topFrames: undefined, stacks: [...this.stacks] }); }

    update() {
        const { stackTraces, withoutFrames } = this.#releaseSamples();

        this.stackTraces += stackTraces.length;
        this.#notYetInStacks.push(stackTraces);
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
        prepare?.();
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

// Most of those that include the last of the names.
function expectMostStacksToInclude(names, step) {
    for (let tally; !(tally?.withStackIncluding(...names) > tally?.withStackIncluding(names.at(-1)) / 2);) {
        failIfOutOfTime(() => `${names}: ${tally}`);
        tally = new Tally().repeatUntil(tally => tally.withStackIncluding(names.at(-1)) >= 30, step);
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
