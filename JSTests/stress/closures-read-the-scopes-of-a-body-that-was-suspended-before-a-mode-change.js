//@ runDefault("--useDollarVM=1", "--useConcurrentJIT=0")
//@ runDefault("--useDollarVM=1", "--useConcurrentJIT=0", "--forceCodeBlockToJettisonDueToOldAge=1")

// One realm, one generator function evaluated twice: two linked executables over one unlinked body. The first body is
// suspended in its loop's first iteration: it has made one environment for the loop's scope. The mode changes (a
// debugger, the type profiler, the control flow profiler). The second executable then runs the body in code for the new
// mode, and is collected. The first body resumes and makes a closure in each of two iterations.
//
// Each closure has to read its own iteration's binding, also once the DFG has compiled it. The DFG folds a closure's
// scope when the scope's SymbolTable has made one environment only (SymbolTable::singleton()), so the environments that
// the body makes after the change have to come from the SymbolTable that made the one before it. They do, because a
// body with a suspended activation is always linked from the unlinked code it first ran in
// (FunctionExecutable::codeOfStartedBody()): its SymbolTables stay the same objects.

const source = "(function* gen(tag) { /* # */ const out = []; const size = () => out.length; while (size() < 2) { let x = tag + size(), unused = 0; if (!size()) yield out; out.push(function read() { return x; }); } yield out; })";
const step = fn => new Promise(resolve => setTimeout(() => resolve(fn()), 0));
const modeChanges = {
    "debugger": [() => $vm.enableDebuggerModeWhenIdle(), () => $vm.disableDebuggerModeWhenIdle()],
    "type profiler": [() => $vm.setTypeProfilerEnabledWhenIdle(true), () => $vm.setTypeProfilerEnabledWhenIdle(false)],
    "control flow profiler": [() => $vm.setControlFlowProfilerEnabledWhenIdle(true), () => $vm.setControlFlowProfilerEnabledWhenIdle(false)],
};

async function closuresAfter(mode) {
    const [turnOn, turnOff] = modeChanges[mode];
    const evaluate = () => (0, eval)(source.replace("#", mode));
    const suspended = evaluate()("a");
    let other = evaluate()("b");
    suspended.next();

    turnOn();
    await step(() => { other.next(); });
    await step(() => {
        other = null;
        // A Baseline compile empties the type profiler's log, which keeps what the other body logged alive.
        const warm = (0, eval)("(function warm(i) { return i + 1; })");
        for (let i = 0, sum = 0; i < testLoopCount; i++)
            sum += warm(i);
    });
    await step(() => { fullGC(); fullGC(); });
    await step(() => { fullGC(); fullGC(); });
    const [inFirstIteration, inSecondIteration] = await step(() => suspended.next().value);
    await step(() => {
        const values = [];
        noInline(inSecondIteration);
        for (let i = 0; i < testLoopCount; i++)
            inSecondIteration();
        values.push(inSecondIteration(), inFirstIteration());
        for (let i = 0; i < testLoopCount; i++)
            inFirstIteration();
        values.push(inFirstIteration(), inSecondIteration());
        if (values.join() !== "a1,a0,a0,a1")
            throw new Error(mode + ": the closures read " + values.join() + ", expected a1,a0,a0,a1");
    });
    turnOff();
    await step(() => { });
}

(async () => {
    for (const mode in modeChanges)
        await closuresAfter(mode);
})().catch(error => {
    print(error);
    $vm.abort();
});
