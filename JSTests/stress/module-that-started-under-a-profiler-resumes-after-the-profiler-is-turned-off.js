//@ requireOptions("-m", "--useDollarVM=1")
//@ runDefault("--useTypeProfiler=1")
//@ runDefault("--useControlFlowProfiler=1")

// A module that is suspended at a top-level await keeps the code generation mode it started in: its registers are in
// a generator frame, like those of a generator's body. This one starts while a profiler is on, so its code has the
// profiler's instructions. The profiler is turned off while the module is suspended, and all code is deleted. The
// module's code is then generated again with those instructions and is linked against a profiler that is made for
// it (VM::typeProfilerForInstrumentedCode).

function check(what, actual, expected) {
    if (actual !== expected)
        throw new Error(what + " is " + actual + ", expected " + expected);
}

const nextTask = () => new Promise(resolve => setTimeout(resolve, 0));
function describe(value) { return typeof value + ":" + value; }

let sum = 1;
let seen = describe(sum);

// Whichever profiler the options turned on, once this module has returned to the event loop.
$vm.setTypeProfilerEnabledWhenIdle(false);
$vm.setControlFlowProfilerEnabledWhenIdle(false);
await nextTask();

for (const value of [2, "three", { four: 4 }]) {
    sum += typeof value === "number" ? value : 10;
    seen = describe(value);
    try {
        await nextTask();
    } catch {
        sum = -1;
    } finally {
        sum += 100;
    }
}
check("sum", sum, 323);
check("seen", seen, "object:[object Object]");
