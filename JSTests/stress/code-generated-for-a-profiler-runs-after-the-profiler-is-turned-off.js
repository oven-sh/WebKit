//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--useJIT=0")
//@ runDefault("--useDollarVM=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=200")

// The body of a generator or an async function that is suspended keeps the code it first ran in, because it only
// resumes in the code that suspended it (ScriptExecutable::newCodeBlockFor). A body that starts while a profiler is on
// therefore still has the profiler's instructions after the profiler is turned off. All code is deleted then, and the
// body is linked again from its code with those instructions: it is linked against a profiler, and it writes into that
// profiler in every tier. So turning a profiler off does not destroy it. It goes with the code that is deleted, and
// code for it that is linked afterwards makes it again (VM::typeProfilerForInstrumentedCode).

function check(what, actual, expected) {
    if (actual !== expected)
        throw new Error(what + " is " + actual + ", expected " + expected);
}

// A profiler is turned on and off the way the inspector does it: once the VM is idle, with all code deleted.
const step = fn => new Promise(resolve => setTimeout(() => resolve(fn()), 0));
const profilers = {
    "type profiler": enabled => $vm.setTypeProfilerEnabledWhenIdle(enabled),
    "control flow profiler": enabled => $vm.setControlFlowProfilerEnabledWhenIdle(enabled),
};

// (The comments keep one function's code apart from the next one's in the CodeCache.)
const shapedSource = "(function* shaped(a) { /* # */ let y = a; let x; x = ++y + (yield 1); let z = x * 2; const w = yield z; return x + y + z + w; })";
const hotSource = "(function* hot(a) { /* # */ let sum = a; let o = { v: 1 }; while (true) { const n = yield sum; for (let i = 0; i < n; i++) { sum += (i & 3) + o.v; if (i % 1000 === 999) o = { v: 1, w: i }; } } })";
// What next(n) adds to the sum of a `hot` body: (i & 3) + 1 for each i below n.
const hotAdds = n => n + 6 * Math.floor(n / 4) + [0, 0, 1, 3][n % 4];
const typedSource = "(function* typed() { /* # */ let seen; while (true) seen = yield; })";

// A body that started under the profiler runs after the profiler is off, and through a second on and off.
async function pinnedBodies(profiler) {
    const setEnabled = profilers[profiler];
    const isControlFlowProfiler = profiler === "control flow profiler";
    const shaped = (0, eval)(shapedSource.replace("#", profiler));
    const hot = (0, eval)(hotSource.replace("#", profiler));
    const typed = (0, eval)(typedSource.replace("#", profiler));
    // What each profiler has for a body, while the profiler is on.
    const timesTheLoopBodyRan = () => $vm.basicBlockExecutionCount(hot, "sum += (i & 3)");
    const typesSeen = () => $vm.findTypeForExpression(typed, "seen = yield").instructionTypeSet.primitiveTypeNames.join();
    let first, loop, values, second;
    let sum = 1;

    setEnabled(true);
    await step(() => {
        // Both start in code generated for the profiler.
        first = shaped(10);
        loop = hot(1);
        check(profiler + " on: first.next()", first.next().value, 1);
        check(profiler + " on: loop.next()", loop.next().value, sum);
        check(profiler + " on: loop.next(10)", loop.next(10).value, sum += hotAdds(10));
        values = typed();
        values.next();
        values.next(1);
        if (isControlFlowProfiler)
            check("on: times the loop body ran", timesTheLoopBodyRan(), 10);
        else
            check("on: types seen", typesSeen(), "Integer");
    });

    setEnabled(false);
    await step(() => {
        check(profiler + " off: first.next(5)", first.next(5).value, 32);
        // Hot enough for every tier.
        check(profiler + " off: loop.next(testLoopCount)", loop.next(testLoopCount).value, sum += hotAdds(testLoopCount));
        check(profiler + " off: loop.next(testLoopCount) again", loop.next(testLoopCount).value, sum += hotAdds(testLoopCount));
        values.next("text");
    });

    // Turned on again, the profiler is the one that the bodies' code is linked against, and it starts empty: what the
    // bodies wrote while it was off is not counted.
    setEnabled(true);
    await step(() => {
        check(profiler + " on again: first.next(7)", first.next(7).value, 66);
        check(profiler + " on again: loop.next(10)", loop.next(10).value, sum += hotAdds(10));
        values.next(true);
        if (isControlFlowProfiler)
            check("on again: times the loop body ran", timesTheLoopBodyRan(), 10);
        else
            check("on again: types seen", typesSeen(), "Boolean");
        second = (0, eval)(shapedSource.replace("#", profiler + ", on again"))(10);
        check(profiler + " on again: second.next()", second.next().value, 1);
    });

    setEnabled(false);
    await step(() => {
        check(profiler + " off again: second.next(5)", second.next(5).value, 32);
        check(profiler + " off again: second.next(7)", second.next(7).value, 66);
        check(profiler + " off again: loop.next(10)", loop.next(10).value, sum += hotAdds(10));
    });
}

// A body that outlived the type profiler still writes into a log, which fills up. What it writes is dropped, in every
// tier, with objects that die in between and structures that nothing else has.
async function bodyThatOutlivedTheTypeProfilerFillsTheLog() {
    const body = (0, eval)("(function* body() { let seen = 0; while (true) { const batch = yield seen; for (let i = 0; i < batch.length; i++) { const each = batch[i]; seen += each.tag; } } })");
    let generator;
    let total = 0;

    $vm.setTypeProfilerEnabledWhenIdle(true);
    await step(() => {
        generator = body();
        generator.next();
    });
    $vm.setTypeProfilerEnabledWhenIdle(false);
    await step(() => {
        const batch = [];
        for (let i = 0; i < 20; i++) {
            const object = { tag: 1 };
            object["only" + i] = i;
            batch.push(object);
        }
        total += batch.length;
        check("the sum after the objects that die", generator.next(batch).value, total);
    });
    await step(() => {
        const warm = (0, eval)("(function warm(x) { return x + 1; })");
        let sum = 0;
        for (let i = 0; i < testLoopCount; i++)
            sum = warm(sum);
    });
    await step(() => fullGC());
    await step(() => fullGC());
    await step(() => {
        // 20,000 iterations of the body's loop: more writes than the log has room for (50,000 entries).
        const batch = [];
        for (let i = 0; i < 100; i++)
            batch.push({ tag: 1 });
        for (let round = 0; round < 200; round++) {
            total += batch.length;
            check("the sum in round " + round, generator.next(batch).value, total);
        }
    });
}

// The type profiler's log keeps no value alive once the profiler is off: not what it holds when the profiler is
// turned off, and not what a body that outlived the profiler writes into it afterwards.
async function logKeepsNothingAliveWhileTheProfilerIsOff() {
    const passThrough = (0, eval)("(function passThrough(value) { return value; })");
    const consume = (0, eval)("(function* consume() { let last; while (true) last = yield; })");
    const loggedWhileOn = [];
    const loggedWhileOff = [];
    const makeLogged = i => ({ payload: new Array(1000).fill(i) });
    let consumer;

    $vm.setTypeProfilerEnabledWhenIdle(true);
    await step(() => {
        consumer = consume();
        consumer.next();
        for (let i = 0; i < 32; i++) {
            const logged = makeLogged(i);
            passThrough(logged);
            loggedWhileOn.push(new WeakRef(logged));
        }
    });
    $vm.setTypeProfilerEnabledWhenIdle(false);
    await step(() => {
        for (let i = 0; i < 32; i++) {
            const logged = makeLogged(i);
            consumer.next(logged);
            loggedWhileOff.push(new WeakRef(logged));
        }
        consumer.next();
    });
    await step(() => fullGC());
    await step(() => {
        check("values alive that were logged while the type profiler was on", loggedWhileOn.filter(ref => ref.deref()).length, 0);
        check("values alive that were logged after the type profiler was turned off", loggedWhileOff.filter(ref => ref.deref()).length, 0);
    });
}

// The type sets of a body's variables belong to its SymbolTables too, and outlive the profiler. The next time the
// profiler is on they have nothing from before: not what the profiler saw the last time, and not what the body ran with
// while the profiler was off, when its code fills the log of a profiler that is gone again before the next time.
async function typeSetsOfVariablesStartEmpty(bodyRunsWhileOff) {
    const typed = (0, eval)(typedSource.replace("#", "variables, " + bodyRunsWhileOff));
    const typesOfTheVariable = () => JSON.stringify($vm.findTypeForExpression(typed, "seen = yield").globalTypeSet);
    let values;

    $vm.setTypeProfilerEnabledWhenIdle(true);
    await step(() => {
        values = typed();
        values.next();
        values.next(1);
        values.next({ only: 1 });
        const types = JSON.parse(typesOfTheVariable());
        check("on: types of the variable", types.primitiveTypeNames.join(), "Undefined,Integer");
        check("on: shapes of the variable", types.structures.length, 1);
    });
    $vm.setTypeProfilerEnabledWhenIdle(false);
    if (bodyRunsWhileOff) {
        await step(() => {
            // More resumes than the log has room for (50,000 entries).
            for (let i = 0; i < 60000; i++)
                values.next("text");
        });
        $vm.deleteAllCodeWhenIdle();
        await step(() => { });
    }
    $vm.setTypeProfilerEnabledWhenIdle(true);
    await step(() => {
        values.next(true);
        check("on again: types of the variable", typesOfTheVariable(), '{"displayTypeName":"Boolean","primitiveTypeNames":["Boolean"],"structures":[]}');
    });
    $vm.setTypeProfilerEnabledWhenIdle(false);
    await step(() => { });
}

(async () => {
    await pinnedBodies("type profiler");
    await pinnedBodies("control flow profiler");
    await bodyThatOutlivedTheTypeProfilerFillsTheLog();
    await logKeepsNothingAliveWhileTheProfilerIsOff();
    await typeSetsOfVariablesStartEmpty(false);
    await typeSetsOfVariablesStartEmpty(true);
})().catch(error => {
    print(error);
    $vm.abort();
});
