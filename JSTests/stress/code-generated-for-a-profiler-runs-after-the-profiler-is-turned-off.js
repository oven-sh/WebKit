//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--useJIT=0")

// The body of a generator or an async function keeps the code generation mode it first ran in, because it only resumes
// in code generated the same way (ScriptExecutable::newCodeBlockFor). A body that starts while a profiler is on
// therefore has the profiler's instructions for as long as it lives. When the profiler is turned off all code is
// deleted, and the body's code is generated again with those instructions: it is linked against the profiler, and it
// writes into the profiler in every tier. So a profiler that was on once stays (VM::typeProfilerForInstrumentedCode).

function check(what, actual, expected) {
    if (actual !== expected)
        throw new Error(what + " is " + actual + ", expected " + expected);
}

function run(profiler, setEnabled, done) {
    // (The comments keep one function's code apart from the next one's in the CodeCache.)
    const shapedSource = "(function* shaped(a) { /* # */ let y = a; let x; x = ++y + (yield 1); let z = x * 2; const w = yield z; return x + y + z + w; })";
    const shaped = (0, eval)(shapedSource.replace("#", profiler));
    const hot = (0, eval)("(function* hot(a) { /* " + profiler + " */ let sum = a; let o = { v: 1 }; while (true) { const n = yield sum; for (let i = 0; i < n; i++) { sum += (i & 3) + o.v; if (i % 1000 === 999) o = { v: 1, w: i }; } } })");

    setEnabled(true);
    setTimeout(() => {
        // Both start in code generated for the profiler.
        const first = shaped(10);
        const loop = hot(1);
        check(profiler + " on: first.next()", first.next().value, 1);
        check(profiler + " on: loop.next()", loop.next().value, 1);
        check(profiler + " on: loop.next(10)", loop.next(10).value, 24);

        setEnabled(false);
        setTimeout(() => {
            check(profiler + " off: first.next(5)", first.next(5).value, 32);
            // Hot enough for every tier, and for the type profiler's log to fill up and be processed.
            check(profiler + " off: loop.next(20000)", loop.next(20000).value, 50024);
            check(profiler + " off: loop.next(20000) again", loop.next(20000).value, 100024);

            // Turned on again, the profiler is the one that code is linked against.
            setEnabled(true);
            setTimeout(() => {
                check(profiler + " on again: first.next(7)", first.next(7).value, 66);
                check(profiler + " on again: loop.next(10)", loop.next(10).value, 100047);
                const second = (0, eval)(shapedSource.replace("#", profiler + ", on again"))(10);
                check(profiler + " on again: second.next()", second.next().value, 1);

                setEnabled(false);
                setTimeout(() => {
                    check(profiler + " off again: second.next(5)", second.next(5).value, 32);
                    check(profiler + " off again: second.next(7)", second.next(7).value, 66);
                    check(profiler + " off again: loop.next(10)", loop.next(10).value, 100070);
                    done();
                }, 0);
            }, 0);
        }, 0);
    }, 0);
}

run("type profiler", enabled => $vm.setTypeProfilerEnabledWhenIdle(enabled), () => {
    run("control flow profiler", enabled => $vm.setControlFlowProfilerEnabledWhenIdle(enabled), () => { });
});
