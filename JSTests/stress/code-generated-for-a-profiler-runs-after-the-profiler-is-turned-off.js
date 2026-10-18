//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--useJIT=0")
//@ runDefault("--useDollarVM=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=200")

// The body of a generator or an async function keeps the code generation mode it first ran in, because it only resumes
// in code generated the same way (ScriptExecutable::newCodeBlockFor). A body that starts while a profiler is on
// therefore has the profiler's instructions for as long as it lives. When the profiler is turned off all code is
// deleted, and the body's code is generated again with those instructions: it is linked against a profiler, and it
// writes into that profiler in every tier. So turning a profiler off does not destroy it. It goes with the code that
// is deleted, and code for it that is linked afterwards makes it again (VM::typeProfilerForInstrumentedCode).

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
const typedSource = "(function* typed() { /* # */ let seen; while (true) seen = yield; })";
// Where the catch block ends and the finally block starts, the control flow profiler has a block that stands for no
// text: a dummy, which code that leaves the catch block counts.
const cleanupSource = "(function* cleanup(a) { /* # */ let n = a; try { n += yield n; } catch { n = -1; } finally { n += 1; } yield n; })";

// A body that started under the profiler runs after the profiler is off, and through a second on and off.
async function pinnedBodies(profiler) {
    const setEnabled = profilers[profiler];
    const isControlFlowProfiler = profiler === "control flow profiler";
    const shaped = (0, eval)(shapedSource.replace("#", profiler));
    const hot = (0, eval)(hotSource.replace("#", profiler));
    const typed = (0, eval)(typedSource.replace("#", profiler));
    const cleanup = (0, eval)(cleanupSource.replace("#", profiler));
    // What each profiler has for a body, while the profiler is on.
    const timesTheLoopBodyRan = () => $vm.basicBlockExecutionCount(hot, "sum += (i & 3)");
    const typesSeen = () => $vm.findTypeForExpression(typed, "seen = yield").instructionTypeSet.primitiveTypeNames.join();
    let first, loop, values, inTry, second;

    setEnabled(true);
    await step(() => {
        // Both start in code generated for the profiler.
        first = shaped(10);
        loop = hot(1);
        check(profiler + " on: first.next()", first.next().value, 1);
        check(profiler + " on: loop.next()", loop.next().value, 1);
        check(profiler + " on: loop.next(10)", loop.next(10).value, 24);
        values = typed();
        values.next();
        values.next(1);
        inTry = cleanup(1);
        check(profiler + " on: inTry.next()", inTry.next().value, 1);
        if (isControlFlowProfiler)
            check("on: times the loop body ran", timesTheLoopBodyRan(), 10);
        else
            check("on: types seen", typesSeen(), "Integer");
    });

    setEnabled(false);
    await step(() => {
        check(profiler + " off: first.next(5)", first.next(5).value, 32);
        // Hot enough for every tier, and for the type profiler's log to fill up and be processed.
        check(profiler + " off: loop.next(20000)", loop.next(20000).value, 50024);
        check(profiler + " off: loop.next(20000) again", loop.next(20000).value, 100024);
        values.next("text");
        check(profiler + " off: inTry.throw()", inTry.throw("caught").value, 0);
    });

    // Turned on again, the profiler is the one that the bodies' code is linked against, and it starts empty: what the
    // bodies wrote while it was off is not counted.
    setEnabled(true);
    await step(() => {
        check(profiler + " on again: first.next(7)", first.next(7).value, 66);
        check(profiler + " on again: loop.next(10)", loop.next(10).value, 100047);
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
        check(profiler + " off again: loop.next(10)", loop.next(10).value, 100070);
    });
}

// The collector goes on treating the type profiler's log and type sets as it does while the profiler is on:
// - a value in the log is alive until the log is processed, so the log is visited (without the JIT it is only
//   processed when it is full),
// - a structure that the processed log put into a type set can die, and is taken out of the set at the end of a
//   collection, because the DFG turns the set into a structure check.
async function collectorAndTypeProfilerOfPinnedBody() {
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
        // Objects whose structures nothing else has.
        const batch = [];
        for (let i = 0; i < 20; i++) {
            const object = { tag: 1 };
            object["only" + i] = i;
            batch.push(object);
        }
        total += batch.length;
        check("the sum after the objects that die", generator.next(batch).value, total);
    });
    // With the JIT, a Baseline compile processes the log here, while the objects are alive.
    await step(() => {
        const warm = (0, eval)("(function warm(x) { return x + 1; })");
        let sum = 0;
        for (let i = 0; i < 2000; i++)
            sum = warm(sum);
    });
    await step(() => fullGC());
    await step(() => fullGC());
    await step(() => {
        // Hot enough for the DFG to compile the body, and for the log to fill up.
        const batch = [];
        for (let i = 0; i < 100; i++)
            batch.push({ tag: 1 });
        for (let round = 0; round < 200; round++) {
            total += batch.length;
            check("the sum in round " + round, generator.next(batch).value, total);
        }
    });
}

// What the type profiler's log holds when the profiler is turned off is not kept alive.
async function logIsNotKeptAfterTheProfilerIsOff() {
    const passThrough = (0, eval)("(function passThrough(value) { return value; })");
    const weak = [];

    $vm.setTypeProfilerEnabledWhenIdle(true);
    await step(() => {
        for (let i = 0; i < 32; i++) {
            const logged = { payload: new Array(1000).fill(i) };
            passThrough(logged);
            weak.push(new WeakRef(logged));
        }
    });
    $vm.setTypeProfilerEnabledWhenIdle(false);
    await step(() => { });
    await step(() => fullGC());
    await step(() => {
        check("logged values alive after the type profiler was turned off", weak.filter(ref => ref.deref()).length, 0);
    });
}

(async () => {
    await pinnedBodies("type profiler");
    await pinnedBodies("control flow profiler");
    await collectorAndTypeProfilerOfPinnedBody();
    await logIsNotKeptAfterTheProfilerIsOff();
})().catch(error => {
    print(error);
    $vm.abort();
});
