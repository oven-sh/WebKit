//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--alwaysUseShadowChicken=1", "--useDollarVM=1")
// The shadow chicken keeps one log and one shadow stack per VM, for the
// debugger's tail-deleted frames. GIL-off, spawned threads must not write the
// log or rebuild the shadow stack, and the collector must not walk a stack
// from the VM-level top call frame, which no thread writes. The main thread
// must still see its own tail-deleted frames, and a spawned thread sees its
// machine frames.
"use strict";

const THREADS = 3;
const ROUNDS = 3000;

function names(stack) {
    return stack.map(f => {
        try {
            return f.name || "?";
        } catch {
            return "?";
        }
    }).join(",");
}

function mainLeaf() { const stack = $vm.shadowChickenFunctionsOnStack(); return stack; }
function mainTail() { return mainLeaf(); }
function mainCaller() { const stack = mainTail(); return stack; }

function spawnedLeaf(n) {
    if (n)
        return spawnedTail(n - 1);
    const stack = $vm.shadowChickenFunctionsOnStack();
    return stack;
}
function spawnedTail(n) { return spawnedLeaf(n); }
function spawnedCaller(n) { const stack = spawnedTail(n); return stack; }

const state = { stop: false };

const threads = [];
for (let i = 0; i < THREADS; ++i) {
    threads.push(new Thread(() => {
        let rounds = 0;
        while (!state.stop) {
            const stack = spawnedCaller(20);
            if (stack[0] !== $vm.shadowChickenFunctionsOnStack || stack.indexOf(spawnedCaller) < 0)
                throw new Error("spawned stack: " + names(stack));
            if (stack.indexOf(mainCaller) >= 0 || stack.indexOf(mainTail) >= 0)
                throw new Error("spawned stack has main-thread frames: " + names(stack));
            ++rounds;
        }
        return rounds;
    }));
}

for (let round = 0; round < ROUNDS; ++round) {
    const stack = mainCaller();
    const text = names(stack);
    if (stack[0] !== $vm.shadowChickenFunctionsOnStack || stack[1] !== mainLeaf || stack[2] !== mainTail || stack[3] !== mainCaller)
        throw new Error("main stack at round " + round + ": " + text);
    if (stack.indexOf(spawnedTail) >= 0 || stack.indexOf(spawnedCaller) >= 0)
        throw new Error("main stack has spawned frames at round " + round + ": " + text);
    if (!(round % 500))
        gc();
}

state.stop = true;
for (const thread of threads)
    thread.join();
