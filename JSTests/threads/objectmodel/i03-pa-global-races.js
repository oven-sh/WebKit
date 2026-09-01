//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: oversized/PA-cell transition suite (I36).
//
// PreciseAllocation cells sit at 8-mod-16 addresses, so the 128-bit
// header+butterfly DCAS would fault and is FORBIDDEN for them (I36); every
// transition/conversion/SW flip on a PA cell is cell-locked and published
// via the M8 fenced nuke order; E4 lock-free owner transitions are excluded.
// The canonical reachable PA object is the JSGlobalObject itself (annex
// §16.8b), so:
//   (a) global-object property races: foreign writes to existing globals +
//       foreign adds of new globals, racing main-thread adds — the I36
//       locked path on a real PA cell;
//   (b) foreign write + owner transition interleaved on the global.
// Invariants: no lost adds/writes (I21), values exact, neighbors untouched.
load("../harness.js", "caller relative");

const THREADS = 4;
const PER = 12;
const ROUNDS = 8;

// (a) Racing adds of DISJOINT new globals from N foreign threads.
for (let round = 0; round < ROUNDS; ++round) {
    const workers = spawnN(THREADS, (t) => {
        for (let k = 0; k < PER; ++k)
            globalThis["g_r" + round + "_t" + t + "_" + k] = t * 1000 + k;
        return true;
    });
    joinAll(workers).forEach((r) => shouldBeTrue(r));

    for (let t = 0; t < THREADS; ++t) {
        for (let k = 0; k < PER; ++k) {
            const name = "g_r" + round + "_t" + t + "_" + k;
            shouldBeTrue(name in globalThis, "round " + round + ": lost global add " + name + " (I36)");
            shouldBe(globalThis[name], t * 1000 + k, "round " + round + ": global value aliased " + name);
        }
    }
}

// (b) Foreign write to an EXISTING global (locked SW flip on the PA cell)
// racing the owner's (main thread's) global transitions.
globalThis.paSharedSlot = "main-initial";
for (let round = 0; round < ROUNDS; ++round) {
    const gate = { go: 0 };
    const writer = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let w = 0; w < 24; ++w) {
            globalThis.paSharedSlot = "foreign-" + round + "-" + w;
            if (!(w & 7))
                sleepMs(1);
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    for (let k = 0; k < PER; ++k) {
        globalThis["g_owner_r" + round + "_" + k] = "own" + k; // owner transition, cell-locked on PA (no E4)
        if (!(k & 3))
            sleepMs(1);
    }
    shouldBeTrue(writer.join());

    shouldBe(globalThis.paSharedSlot, "foreign-" + round + "-23",
        "round " + round + ": foreign global write lost across owner transition (I36)");
    for (let k = 0; k < PER; ++k)
        shouldBe(globalThis["g_owner_r" + round + "_" + k], "own" + k,
            "round " + round + ": owner global add lost across foreign flip (I36)");
}

// (c) Foreign DELETE of a global + re-add, racing reads (quarantine on a PA
// cell's table; D1 makes the tardy read old-value-or-undefined).
globalThis.paVictim = "victim-value";
{
    const reader = new Thread(() => {
        for (let s = 0; s < 300; ++s) {
            const v = globalThis.paVictim;
            if (v !== "victim-value" && v !== undefined && v !== "victim-readded")
                throw new Error("PA delete race read foreign value " + describe(v) + " (I36/I18)");
        }
        return true;
    });
    const deleter = new Thread(() => {
        delete globalThis.paVictim;
        for (let i = 0; i < 8; ++i)
            globalThis["paFiller" + i] = i; // tempt slot reuse pre-epoch
        globalThis.paVictim = "victim-readded";
    });
    shouldBeTrue(reader.join());
    deleter.join();
    shouldBe(globalThis.paVictim, "victim-readded");
    for (let i = 0; i < 8; ++i)
        shouldBe(globalThis["paFiller" + i], i);
}
