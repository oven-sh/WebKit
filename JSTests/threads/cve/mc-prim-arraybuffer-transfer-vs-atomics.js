//@ requireOptions("--useJSThreads=1")
// MC-PRIM susceptibility test (docs/threads/cve/map-MC-PRIM.md, surface P6).
//
// Trusted-primitive invariant bypass on raw buffer primitives: Atomics RMW
// on a typed array and the memmove-class fast paths (fill) perform raw
// loads/stores trusting a {base, length, !detached} tuple validated at entry.
// Under the shared heap a NON-shared ArrayBuffer is reachable from two
// Threads, so detach/transfer/resize machinery on thread B can break that
// invariant while thread A is between its check and its raw access - the
// exact CVE-2012-0507/Unsafe shape (the atomic op is the least-checked
// store). SPEC-ungil §N.6 + annex N6 rule the torn-pair table: a racing
// reader must NEVER pair a passing length with an unmapped-or-short base
// (DETACH: length=0 seq_cst first, contents QUARANTINED to a heap §10 stop;
// TRANSFER = copy + source detach; SHRINK: length seq_cst, tail free
// deferred; GROW: base immutable, commit then release-publish length).
// Audit rows: SPEC-ungil-audit-N7.md R10 (ArrayBuffer.h:199/:298), R11
// (JSArrayBufferView m_vector/m_length/m_mode).
//
// Probe: thread A hammers Atomics.add/load/store and fill on an Int32Array;
// the main thread transfers (detach) and, where supported, resizes the
// backing buffer in a loop, re-arming A with a fresh buffer each round via a
// shared box. Legal outcomes per op: success against a live snapshot, or
// TypeError (detached/out-of-bounds). Susceptibility = anything else:
// values that were never written (read through a freed/short base), a
// RangeError/crash from inside the raw path, or ASAN/TSAN hits (UAF on
// quarantine-bypassing free). This cannot deterministically prove the torn
// pair - it is the amplifier-ready hammer for the §N.6 windows; run under
// ASAN/TSAN post-ungil.
load("../harness.js", "caller relative");

const ROUNDS = 60;
const HAMMER = 400;
const MARK = 0x5a5a5a5a | 0;

const box = { ta: null, round: 0, stop: 0 }; // Shared rendezvous.

const hammer = new Thread(() => {
    let lastRound = -1;
    let badValues = 0;
    while (Atomics.load(box, "stop") === 0) {
        const round = Atomics.load(box, "round");
        if (round === lastRound) {
            sleepMs(1);
            continue;
        }
        lastRound = round;
        const ta = Atomics.load(box, "ta");
        if (!ta)
            continue;
        for (let i = 0; i < HAMMER; ++i) {
            try {
                // Raw RMW + raw read + memmove-class write, all racing the
                // main thread's transfer/resize of ta.buffer.
                Atomics.add(ta, i % 4, 1);
                const v = Atomics.load(ta, i % 4);
                // Every in-bounds word only ever holds MARK + small deltas
                // (fill rewrites MARK; adds bump it by <= 2*HAMMER). A value
                // outside that band was read through a stale/foreign base.
                const delta = (v - MARK) | 0;
                if (delta < 0 || delta > 2 * HAMMER)
                    ++badValues;
                ta.fill(MARK);
            } catch (e) {
                if (!(e instanceof TypeError))
                    throw new Error("non-TypeError out of a raw buffer primitive (round " + round + "): " + e);
                break; // Detached: wait for the next round's buffer.
            }
        }
    }
    return badValues;
});

const canResize = typeof ArrayBuffer.prototype.resize === "function";
const canTransfer = typeof ArrayBuffer.prototype.transfer === "function";

for (let r = 0; r < ROUNDS; ++r) {
    const ab = canResize ? new ArrayBuffer(64, { maxByteLength: 4096 }) : new ArrayBuffer(64);
    const ta = new Int32Array(ab); // length-tracking when resizable
    ta.fill(MARK);
    Atomics.store(box, "ta", ta);
    Atomics.store(box, "round", r + 1);
    // Let the hammer land mid-flight, then break the invariant under it.
    for (let k = 0; k < 20; ++k) {
        if (canResize) {
            ab.resize(4096); // GROW: base immutable, commit-then-publish length.
            ab.resize(32);   // SHRINK: length seq_cst first, tail free deferred.
        }
        Atomics.add(ta, 0, 1); // Keep contention on the same words.
    }
    if (canTransfer)
        ab.transfer(); // TRANSFER = copy + source DETACH: len=0 seq_cst, contents quarantined.
}
Atomics.store(box, "stop", 1);
const badValues = hammer.join();
shouldBe(badValues, 0, "no value ever read through a stale/short base (torn {base,length} pair)");
