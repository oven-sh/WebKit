//@ requireOptions("-m")
//@ runDefault
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=200")

// As suspended-activation-keeps-scope-inference-when-code-is-generated-again.js for a module that is suspended at a
// top-level await in the body of a loop, while all code is generated again for the debugger and, at the next await, once
// more without it. The module's own code has to be generated the way it was the first time (like a generator's body: its
// registers are in a generator frame, and code that captures every variable for the debugger does not find them there),
// and the closures made in the loop after it resumed have to read their own iterations' bindings.

const iterations = 4;
const calls = testLoopCount;
const failures = [];

let open = [];
function gate() { return new Promise(resolve => { open.push(resolve); }); }

function later(step) {
    setTimeout(() => {
        fullGC();
        setTimeout(step, 0);
    }, 0);
}

$vm.enableDebuggerModeWhenIdle();
later(() => {
    open.shift()(); // finishes iteration 0 and suspends in iteration 1
    setTimeout(() => {
        $vm.disableDebuggerModeWhenIdle();
        later(() => { open.shift()(); });
    }, 0);
});

// The module's own scope: a binding that is assigned again after the resume, read by a function that was compiled while it
// still had its first value.
let topLevel = -1;
function readTopLevel() { return topLevel; }
for (let k = 0; k < calls; k++) {
    if (readTopLevel() !== -1)
        throw new Error("readTopLevel() before the first await");
}

const readers = [];
for (let iter = 0; iter < iterations; iter++) {
    const box = { iter };
    let local = iter * 2; // in a register, which code generated for the debugger would look for in the scope
    if (iter < 2)
        await gate();
    local++;
    readers.push(() => box.iter);
    let wrong = local === iter * 2 + 1 ? 0 : 1;
    for (let k = 0; k < calls; k++) {
        if (readTopLevel() !== iter - 1)
            wrong++;
    }
    topLevel = iter;
    for (let k = 0; k < calls; k++) {
        if (readTopLevel() !== iter)
            wrong++;
    }
    for (let k = 0; k < calls; k++) {
        if ([0].reduce(acc => acc + box.iter, 0) !== iter)
            wrong++;
    }
    // The closures of the earlier iterations, which share this one's code, over their own environments.
    for (let k = 0; k < calls; k++) {
        for (let earlier = 0; earlier <= iter; earlier++) {
            if (readers[earlier]() !== earlier)
                wrong++;
        }
    }
    if (wrong)
        failures.push(wrong + " wrong reads in iteration " + iter);
}
if (failures.length)
    throw new Error(failures.join("; "));
