//@ requireOptions("--useJSThreads=1", "--destroy-vm", "--collectContinuously=true")
//@ threadsRequireGILOff
// With the GIL off, the heap is shared, and a mutator serves each collection
// request by conducting it. The collectContinuously thread requests
// collections. When the VM was destroyed, a request could still be pending,
// and the destroying thread, the last mutator, waited for it after giving up
// its heap access. Nothing served it, and the process hung at exit.
let count = 0;
for (const x of new Uint8Array(10000))
    count += x + 1;
if (count !== 10000)
    throw new Error("bad count " + count);
