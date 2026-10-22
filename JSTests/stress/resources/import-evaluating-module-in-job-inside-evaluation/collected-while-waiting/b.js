log.push("b1");
track("a", import("./a.js"));
drainMicrotasks();
// Only the VM has the promise of that Evaluate() now. A collection must keep it, and these allocations would take
// its cell.
fullGC();
globalThis.promises = [];
for (let i = 0; i < 20000; i++)
    promises.push(new Promise(() => { }));
fullGC();
log.push(`b2(a=${states.a})`);
