//@ requireOptions("--useJSThreads=1")
// MC-DF S2 (docs/threads/cve/map-MC-DF.md): TypedArray/DataView fast paths
// load LENGTH, bounds-check, then load BASE (two fetches, no ordering on the
// reader side). A second agent detaching / transferring / shrinking /
// re-growing the backing ArrayBuffer between the two fetches is the exact
// double-fetch shape of the Bochspwn corpus and CVE-2018-4222-adjacent
// detach races. Governed by SPEC-ungil annex N6 (BINDING): any observable
// base must point at a mapping sized >= every length still observable
// against it (mapping quarantine to the next heap stop; grow keeps the base
// immutable via reserved VA).
//
// Susceptibility oracle: a reader must only ever observe the sentinel byte,
// 0 (freshly committed / zeroFill'd pages), or undefined (bounds-fail /
// detached). ANY other value is a torn {length, base} pair = N6 violation.
// A crash (unmapped base under a passing length) is the CVE-grade outcome.
//
// EXECUTED POST-UNGIL ONLY (written during bring-up; do not run against the
// phase-1 GIL tree expecting signal — under the GIL it is trivially green).
// Amplifier-ready: Tools/threads/amplify.sh + the TSAN no-JIT target.
load("../harness.js", "caller relative");

const SENTINEL = 0xab;
const MAX_BYTES = 1 << 16;
const SMALL_BYTES = 1 << 8;
const ROUNDS = 200;
const READERS = 3;

const shared = { buf: null, view: null, dv: null, round: 0, stop: 0, started: 0 };

function makeBuffer() {
    const ab = new ArrayBuffer(MAX_BYTES, { maxByteLength: MAX_BYTES });
    const ta = new Uint8Array(ab);
    ta.fill(SENTINEL);
    return { ab, ta, dv: new DataView(ab) };
}

const readers = spawnN(READERS, () => {
    Atomics.add(shared, "started", 1);
    let checks = 0;
    while (Atomics.load(shared, "stop") === 0) {
        const ta = shared.view;
        const dv = shared.dv;
        if (!ta) continue;
        // Stride across the whole max range: indexes both below and above
        // any concurrently-published shrink length.
        for (let i = 0; i < MAX_BYTES; i += 977) {
            const v = ta[i]; // TA fast path: length fetch, then base fetch.
            if (v !== SENTINEL && v !== 0 && v !== undefined)
                throw new Error("torn TA read: ta[" + i + "] = " + v);
            checks++;
        }
        // DataView path (separate length-getter machinery): throws on OOB /
        // detached — both acceptable; a garbage byte is not.
        try {
            const w = dv.getUint8((checks * 977) % MAX_BYTES);
            if (w !== SENTINEL && w !== 0)
                throw new Error("torn DataView read: " + w);
        } catch (e) {
            if (e instanceof RangeError || e instanceof TypeError) { /* detached/OOB: fine */ }
            else throw e;
        }
        Atomics.wait(shared, "stop", 0, 1); // bounded yield
    }
    return checks;
});

waitUntil(() => Atomics.load(shared, "started") === READERS);

// Main: the N6 mutation storm — shrink, re-grow, transfer, detach.
for (let r = 0; r < ROUNDS; ++r) {
    const { ab, ta, dv } = makeBuffer();
    shared.buf = ab;
    shared.view = ta;
    shared.dv = dv;
    Atomics.store(shared, "round", r + 1); // publish

    ab.resize(SMALL_BYTES);          // N6 arm 3: shrink (tail quarantined)
    ab.resize(MAX_BYTES);            // N6 arm 4: re-grow in place (VA reserved)
    new Uint8Array(ab).fill(SENTINEL); // re-sentinel committed pages
    ab.resize(SMALL_BYTES);
    if (r & 1)
        ab.transfer(SMALL_BYTES);    // N6 arm 2: copy + detach (source quarantined)
    else if (typeof transferArrayBuffer === "function")
        transferArrayBuffer(ab);     // shell detach helper, if present
    else
        ab.transfer();               // detach-by-transfer fallback
}
Atomics.store(shared, "stop", 1);
Atomics.notify(shared, "stop");

const counts = joinAll(readers);
for (const c of counts)
    shouldBeTrue(c > 0, "reader made progress");
