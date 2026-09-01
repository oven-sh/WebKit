//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-VAL susceptibility test (docs/threads/cve/map-MC-VAL.md, surface V5):
// TID-namespace validator/consumer disagreement after §D.1 reissue.
//
// Validator: conductTIDRebiasUnderSharedStop (Heap.cpp) walks every live
// JSObject world-stopped and proves "no instance carries a dead TID in its
// butterfly tag" before phase-3 reissues those TIDs (ANNEX D1/D1R).
// Consumer: the E4 owner predicate on a FRESH thread holding a reissued
// TID — `g_jscButterflyTIDTag == taggedButterflyWord.tid` ⇒ lock-free
// owner transition (OM E4/I11/I15). If the validator's walk missed any
// instance (or D1R item 1 missed a baked-immediate holder), a reissued
// thread aliases as the dead allocator's "owner" and takes E4 lock-free
// while a true foreign thread takes the locked path on the SAME object —
// the false-owner hazard the V5 tripwire chartered.
//
// Oracle (deterministic in outcome, storm-shaped in schedule): every slot
// encodes its (key, writer) pair; post-storm every slot must decode to
// SOME writer's stamp for ITS key — never another key's value, never a
// torn/garbage word, never a lost final-phase write. A false-owner E4
// races the locked path's nuke-CAS protocol (M5/I9), which surfaces as
// cross-slot bleed, lost transitions, or an I15 debug assert.
//
// GIL-OFF ONLY: GIL-on retired TIDs never recycle (Dev 10) and the
// premise is unconstructible. Heavy (≈13k OS thread spawn/joins to reach
// the 75% per-partition trigger — no Options knob lowers it). Companion
// to mc-tdwn-tid-recycle-storm.js (U-T12 arm 1: SD9 recovery + read
// integrity); this test is the arm-3-adjacent E4-contention half. The
// fully-instrumented D1R item 5 arm (assert specialized CodeBlock
// jettisoned in-stop) remains a non-corpus deferred deliverable per the
// ThreadManager.h banner.
//
// Amplifier-ready (RaceAmplifier::perturb stall points sit pre-walk /
// post-restamp / post-fire in conductTIDRebiasUnderSharedStop). Bounded;
// every thread joined; annex-T2 conventions.
load("../harness.js", "caller relative");

if (typeof $vm !== "undefined" && $vm.useThreadGIL && $vm.useThreadGIL()) {
    // PREMISE-SKIP: Dev 10 holds GIL-on; reissue cannot occur.
    print("PREMISE-SKIP: TID reissue is gilOffProcess-only (Dev 10).");
} else {

const SPAWNED_CAP = 0x4000 - 1;               // [1, carrierTIDBase) — ThreadManager.h
const TRIGGER = ((SPAWNED_CAP * 3) >> 2) + 64; // just past the 75% per-partition arm
const KEEP_EVERY = 192;
const ENC = (key, writer) => (key * 1000) + writer; // key in [0,31], writer in [0,999]

// ---- phase 1: produce dead-TID-tagged keepsakes and drive past the trigger
const keepsakes = [];
let spawned = 0;
while (spawned < TRIGGER) {
    const batch = [];
    const base = spawned;
    for (let i = 0; i < 32; ++i) {
        const wantKeep = ((base + i) % KEEP_EVERY) === 0;
        batch.push(new Thread((n, keep) => {
            // Allocator-owned object: this thread's TID lands in the
            // butterfly instance tag AND in the structure's transition-TLS
            // TID (per-thread structure lineage via the keyed property).
            const o = {};
            o["k" + (n & 7)] = n;        // out-of-line transition keyed on n
            o.s0 = (0 * 1000) + 999;     // ENC(0, 999) — allocator stamp
            o.s1 = (1 * 1000) + 999;
            o[0] = (16 * 1000) + 999;    // indexed butterfly too
            o[1] = (17 * 1000) + 999;
            return keep ? { o, n } : null;
        }, base + i, wantKeep));
    }
    for (let i = 0; i < 32; ++i) {
        try {
            const r = batch[i].join();
            if (r) keepsakes.push(r);
        } catch (e) {
            // SD9 exhaustion can surface here if a prior cycle's rebias is
            // still in-flight; treat as the TDWN test does (bounded retry
            // is phase 2's job — here we just stop producing).
            if (!(e instanceof RangeError)) throw e;
        }
    }
    spawned += 32;
}
shouldBeTrue(keepsakes.length > 0, "produced dead-TID-tagged keepsakes");

// ---- phase 2: force rebias to COMPLETE (seal -> full-stop restamp+fire ->
// reissue) by spawning until the SD9 RangeError gate has lifted at least
// once with retired TIDs in the pipeline. The spawn host call requests the
// full collection when a Sealed snapshot is pending (Heap.cpp:4121).
let reissuedProbe = false;
for (let attempt = 0; attempt < 400 && !reissuedProbe; ++attempt) {
    try {
        const t = new Thread(() => 1);
        t.join();
        // A successful spawn after >=TRIGGER consumed means either (a) we
        // never hit exhaustion (continuous-recycle regime — rebias already
        // ran) or (b) the gate just lifted. Either way reissue is live.
        reissuedProbe = true;
    } catch (e) {
        if (!(e instanceof RangeError)) throw e;
        sleepMs(10);
    }
}
shouldBeTrue(reissuedProbe, "rebias completed and reissue is live");

// ---- phase 3: false-owner contention. FRESH threads (reissued TIDs) and
// MAIN concurrently write the SAME slots on every keepsake. If any
// keepsake's instance tag was NOT restamped to 0, exactly one fresh
// thread's TLS tag aliases it and that thread takes E4 lock-free while
// the others take the foreign locked/segmented path — racing M5/I9.
const WRITERS = 6;
const ROUNDS = 40;
const writerBody = (keeps, me) => {
    const enc = (key, w) => (key * 1000) + w;
    for (let r = 0; r < 40; ++r) {
        for (const k of keeps) {
            const o = k.o;
            o.s0 = enc(0, me);
            o.s1 = enc(1, me);
            o[0] = enc(16, me);
            o[1] = enc(17, me);
            // Transition under contention: a false owner would E4 this
            // lock-free against a foreign locked transitioner.
            o["w" + me] = enc(8 + me, me);
        }
        // G23/G24 cooperative yield (bounded property-path park) so a
        // GIL-on misconfiguration cannot starve — same repair shape as
        // V1/V8 rows.
        if ((r & 15) === 15)
            Atomics.wait(globalThis.__mcvalYield ||= { y: 0 }, "y", 0, 1);
    }
    return me;
};
const writers = [];
for (let w = 0; w < WRITERS; ++w)
    writers.push(new Thread(writerBody, keepsakes, w));
// Main participates as writer 100 (foreign to every keepsake by
// construction: main is TID 0, never reissued, never a keepsake allocator).
for (let r = 0; r < ROUNDS; ++r) {
    for (const k of keepsakes) {
        k.o.s0 = ENC(0, 100);
        k.o.s1 = ENC(1, 100);
        k.o[0] = ENC(16, 100);
        k.o[1] = ENC(17, 100);
    }
}
for (let w = 0; w < WRITERS; ++w)
    shouldBe(writers[w].join(), w, "writer " + w + " ran to completion");

// ---- oracle: every contended slot decodes to (itsKey, someWriter). A
// false-owner E4 racing the locked path manifests as a wrong-key value
// (cross-slot bleed via a torn transition), garbage, or a missing
// per-writer transition.
const legalWriters = new Set([100, 999]);
for (let w = 0; w < WRITERS; ++w) legalWriters.add(w);
function checkSlot(v, key, where) {
    if (typeof v !== "number")
        throw new Error("MC-VAL V5: non-number at " + where + " (torn/garbage): " + String(v));
    const k = (v / 1000) | 0;
    const w = v % 1000;
    if (k !== key)
        throw new Error("MC-VAL V5: cross-slot bleed at " + where + ": expected key " + key + ", got " + k + " (writer " + w + ")");
    if (!legalWriters.has(w))
        throw new Error("MC-VAL V5: unknown writer stamp at " + where + ": " + w);
}
for (const k of keepsakes) {
    checkSlot(k.o.s0, 0, "s0");
    checkSlot(k.o.s1, 1, "s1");
    checkSlot(k.o[0], 16, "[0]");
    checkSlot(k.o[1], 17, "[1]");
    for (let w = 0; w < WRITERS; ++w) {
        // Per-writer transition MUST have landed (E4-vs-locked lost
        // transition is the false-owner failure mode).
        shouldBe(k.o["w" + w], ENC(8 + w, w),
            "per-writer transition w" + w + " landed on keepsake n=" + k.n);
    }
}

} // gilOff-only
