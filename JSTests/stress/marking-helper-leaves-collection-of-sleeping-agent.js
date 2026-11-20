//@ skip if $memoryLimited
//@ runDefault("--numberOfGCMarkers=2")

// A marking helper does not stay in a collection whose conductor sleeps. All heaps of the process share the helpers,
// and with two markers there is one helper.
//
// The agent allocates in steps. Between two steps it sleeps in Atomics.wait, with heap access and with the conn of the
// collection that its allocation started, so that collection does not end while it sleeps.
//
// In each of these sleeps the main thread makes about 300 MB of garbage and gives the conn of its own collections to
// its collector thread (flashHeapAccess), which waits for the helper to mark. If the collection of the agent keeps the
// helper, the collection of the main heap does not end, and the main heap grows by all of the garbage.

const sab = new SharedArrayBuffer(16);
const ia = new Int32Array(sab);
const steps = 10;
const capacityLimit = 300 * 1024 * 1024;

$.agent.start(`
    $.agent.receiveBroadcast(sab => {
        const ia = new Int32Array(sab);
        const make = n => {
            const out = [];
            for (let i = 0; i < n; i++)
                out.push({ i, a: [i, i + 1], s: "x" + i });
            return out;
        };
        globalThis.keep = make(200000);
        globalThis.fresh = [];
        for (let step = 1; step <= ${steps}; step++) {
            fresh.push(make(20000));
            Atomics.store(ia, 1, step);
            Atomics.notify(ia, 1);
            while (Atomics.load(ia, 0) !== step)
                Atomics.wait(ia, 0, step - 1);
        }
        $.agent.report("done");
        $.agent.leaving();
    });
`);
$.agent.broadcast(sab);

globalThis.live = [];
for (let i = 0; i < 300000; i++)
    live.push({ i, a: [i], b: { i } });
fullGC();

let maxCapacity = 0;
for (let step = 1; step <= steps; step++) {
    while (Atomics.load(ia, 1) !== step)
        Atomics.wait(ia, 1, step - 1, 10);
    for (let round = 0; round < 300; round++) {
        let junk = [];
        for (let i = 0; i < 10000; i++)
            junk.push({ i, a: [i, i + 1] });
        // New values in old objects: the collection has work while the program runs.
        for (let i = 0; i < 500; i++)
            live[(round * 7919 + i * 31) % live.length].a = [round, i];
        flashHeapAccess(0);
        maxCapacity = Math.max(maxCapacity, heapCapacity());
        if (maxCapacity > capacityLimit)
            throw new Error("The main heap grew to " + (maxCapacity >> 20) + " MB in step " + step + ": its collection did not end");
    }
    Atomics.store(ia, 0, step);
    Atomics.notify(ia, 0);
}
while (!$.agent.getReport())
    $.agent.sleep(1);
