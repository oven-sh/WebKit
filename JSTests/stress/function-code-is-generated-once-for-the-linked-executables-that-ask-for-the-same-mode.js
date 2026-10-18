//@ runDefault("--useDollarVM=1", "--countParseTimes=1")
//@ runDefault("--useDollarVM=1", "--countParseTimes=1", "--forceCodeBlockToJettisonDueToOldAge=1")

// How often the code of a function is generated. Generating it parses the function once, so parses are counted.
//
// Linked executables that share an unlinked function and ask for the same code generation mode share its code: the
// first one generates it, and the others find it, as does every CodeBlock that is made again later. That holds in
// every mode, for a call and for a construct. An executable that asks for another mode than the one of the code
// that is held gets code of its own, and generates it for each CodeBlock it makes: the held code is not replaced.
// The control flow profiler's part of the mode is the exception: when an embedder turns that profiler on and keeps the
// code that exists, a function that has code keeps running it.

const everyCollectionTakesTheCodeBlocks = jscOptions().forceCodeBlockToJettisonDueToOldAge;
const otherRealm = createGlobalObject();
const failures = [];

function parsesOfCalling(fn, construct) {
    const before = $vm.parseCount();
    if (construct)
        new fn();
    else
        fn(1);
    return $vm.parseCount() - before;
}

function check(what, actual, expected) {
    if (actual !== expected)
        failures.push(what + ": " + actual + " parses, expected " + expected);
}

// One source, evaluated once for each name: a function that is called and one that is constructed, for each evaluation.
function link(tag, realms) {
    const source = "return [function called(a) { /* " + tag + " */ return a + 1; }, function Constructed() { /* " + tag + " */ this.a = 1; }]";
    return realms.map(realm => new realm.Function(source)());
}

const step = fn => new Promise(resolve => setTimeout(() => resolve(fn()), 0));
const collect = () => { fullGC(); fullGC(); };

(async () => {
    // No debugger, no profiler.
    {
        const [first, second] = link("one mode", [globalThis, globalThis]);
        check("the first executable, call", parsesOfCalling(first[0]), 1);
        check("the second executable, call", parsesOfCalling(second[0]), 0);
        check("the first executable, construct", parsesOfCalling(first[1], true), 1);
        check("the second executable, construct", parsesOfCalling(second[1], true), 0);
        await step(collect);
        await step(() => {
            for (const [called, Constructed] of [first, second]) {
                check("a CodeBlock that is made again, call", parsesOfCalling(called), 0);
                check("a CodeBlock that is made again, construct", parsesOfCalling(Constructed, true), 0);
            }
        });
    }

    // A debugger on this realm: two executables that ask for code with the debug hooks, one in a realm without it.
    {
        const [mine, mineToo, theirs] = link("debugger", [globalThis, globalThis, otherRealm]);
        $vm.enableDebuggerModeWhenIdle();
        await step(() => {
            check("debugger on: the first executable, call", parsesOfCalling(mine[0]), 1);
            check("debugger on: the second executable, call", parsesOfCalling(mineToo[0]), 0);
            check("debugger on: the other realm's executable, call", parsesOfCalling(theirs[0]), 1);
            check("debugger on: the first executable, construct", parsesOfCalling(mine[1], true), 1);
            check("debugger on: the second executable, construct", parsesOfCalling(mineToo[1], true), 0);
            check("debugger on: the other realm's executable, construct", parsesOfCalling(theirs[1], true), 1);
        });
        await step(collect);
        await step(() => {
            // The held code is still the debugged realm's. The other realm generates its own again.
            check("debugger on: a CodeBlock that is made again, call", parsesOfCalling(mine[0]) + parsesOfCalling(mineToo[0]), 0);
            check("debugger on: a CodeBlock that is made again, construct", parsesOfCalling(mine[1], true) + parsesOfCalling(mineToo[1], true), 0);
            check("debugger on: the other realm's CodeBlock that is made again, call", parsesOfCalling(theirs[0]), everyCollectionTakesTheCodeBlocks ? 1 : 0);
            check("debugger on: the other realm's CodeBlock that is made again, construct", parsesOfCalling(theirs[1], true), everyCollectionTakesTheCodeBlocks ? 1 : 0);
        });
        $vm.disableDebuggerModeWhenIdle();
        await step(() => { });
    }

    // The type profiler.
    {
        const [first, second] = link("type profiler", [globalThis, globalThis]);
        $vm.setTypeProfilerEnabledWhenIdle(true);
        await step(() => {
            check("type profiler on: the first executable, call", parsesOfCalling(first[0]), 1);
            check("type profiler on: the second executable, call", parsesOfCalling(second[0]), 0);
            check("type profiler on: the first executable, construct", parsesOfCalling(first[1], true), 1);
            check("type profiler on: the second executable, construct", parsesOfCalling(second[1], true), 0);
        });
        await step(collect);
        await step(() => {
            check("type profiler on: a CodeBlock that is made again", parsesOfCalling(first[0]) + parsesOfCalling(second[1], true), 0);
        });
        $vm.setTypeProfilerEnabledWhenIdle(false);
        await step(() => { });
    }

    // The control flow profiler, turned on with the code that exists kept.
    {
        const [first, second] = link("control flow profiler", [globalThis, globalThis]);
        check("before the control flow profiler: the first executable, call", parsesOfCalling(first[0]), 1);
        check("before the control flow profiler: the first executable, construct", parsesOfCalling(first[1], true), 1);
        $vm.setControlFlowProfilerEnabledWhenIdle(true, false);
        await step(() => {
            check("control flow profiler on, code kept: the second executable, call", parsesOfCalling(second[0]), 0);
            check("control flow profiler on, code kept: the second executable, construct", parsesOfCalling(second[1], true), 0);
        });
        await step(collect);
        await step(() => {
            check("control flow profiler on, code kept: a CodeBlock that is made again", parsesOfCalling(first[0]) + parsesOfCalling(second[0]) + parsesOfCalling(first[1], true) + parsesOfCalling(second[1], true), 0);
        });
        $vm.setControlFlowProfilerEnabledWhenIdle(false);
        await step(() => { });
    }

    if (failures.length)
        throw new Error(failures.join("\n"));
})().catch(error => {
    print(error);
    $vm.abort();
});
