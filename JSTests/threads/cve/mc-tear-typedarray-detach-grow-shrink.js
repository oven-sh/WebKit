//@ requireOptions("--useJSThreads=1")
// MC-TEAR S4 (docs/threads/cve/map-MC-TEAR.md): TypedArray/ArrayBuffer
// {base, length} torn-pair susceptibility under a detach / transfer /
// resize-shrink / re-grow storm. Targets UNGIL-HANDOUT §N.6 / annex N6:
// every tier's TA fast path loads LENGTH, bounds-checks, then loads BASE
// with no ordering between the two loads; the invariant is that ANY
// observable base maps a region >= every length still observable against
// it (quarantine-to-stop retirement; grow = commit pages then
// release-publish length).
//
// Oracle (deterministic value membership, race amplified by simultaneity):
// element i of a live region only ever holds SENTINEL(i) (written before
// publication to readers) or 0 (grow zero-fill / fresh pages). A reader may
// also observe detached behavior (undefined element reads, byteLength 0) or
// an OOB-index undefined. ANY other value is a torn {length, base} pair
// (read past a shrunk/retired mapping) => fail. Crashes/ASAN faults are the
// primary signal post-ungil; run under TSAN and Tools/threads/amplify.sh.
//
// WRITTEN DURING BRING-UP: do not execute until the GIL-off ladder is up.
load("../harness.js", "caller relative");

const READERS = 3;
const ROUNDS = 40;
const MAX_LEN = 1 << 16;      // 64 KiB max reservation per buffer
const MIN_LEN = 1 << 8;

function SENTINEL(i) { return (i * 7 + 13) & 0xff; }

// Shared mailbox: main publishes the current victim view; readers hammer it.
const box = { view: null, round: 0, stop: 0, started: 0, errors: null };
const gate = { go: 0 };

const readers = spawnN(READERS, (id) => {
    Atomics.add(box, "started", 1);
    let observed = 0;
    while (Atomics.load(box, "stop") === 0) {
        const ta = box.view; // may be mid-storm, detached, resized
        if (!ta) {
            Atomics.wait(gate, "go", 0, 1);
            continue;
        }
        // Hammer the torn-pair shape: load length, then index near the
        // boundary — exactly the two-load fast path N6 protects.
        for (let k = 0; k < 64; ++k) {
            const len = ta.length;          // load LENGTH
            if (len === 0)
                continue;                   // detached or shrunk-to-min view state
            const i = len - 1 - (k % 8);    // bounds-check passes against len
            if (i < 0)
                continue;
            const v = ta[i];                // load BASE + deref
            // Membership oracle: sentinel, zero-fill, or detached undefined.
            if (v === undefined || v === 0 || v === SENTINEL(i)) {
                observed++;
                continue;
            }
            throw new Error("MC-TEAR S4: torn {length,base} pair: ta[" + i
                + "] = " + v + " (len " + len + ", round "
                + Atomics.load(box, "round") + ", reader " + id + ")");
        }
        // DataView lane: same pair through a different read path. Detached
        // or shrunk-under-us throws RangeError/TypeError — both are the
        // CORRECT bounds-fail arm of the N6 torn-pair table.
        try {
            const buf = ta.buffer;
            const dv = new DataView(buf);
            const bl = dv.byteLength;
            if (bl >= 4) {
                const w = dv.getUint32(bl - 4, true);
                for (let b = 0; b < 4; ++b) {
                    const byte = (w >>> (8 * b)) & 0xff;
                    const idx = bl - 4 + b;
                    if (byte !== 0 && byte !== SENTINEL(idx))
                        throw new Error("MC-TEAR S4 (DataView): torn read at "
                            + idx + ": " + byte);
                }
            }
        } catch (e) {
            if (!(e instanceof RangeError || e instanceof TypeError
                  || String(e.message || "").startsWith("MC-TEAR")))
                throw e;
            if (String(e.message || "").startsWith("MC-TEAR"))
                throw e;
        }
    }
    return observed;
});

waitUntil(() => Atomics.load(box, "started") === READERS);

// Main: the N6 write-arm storm.
for (let r = 0; r < ROUNDS; ++r) {
    Atomics.store(box, "round", r);
    const ab = new ArrayBuffer(MAX_LEN, { maxByteLength: MAX_LEN });
    const ta = new Uint8Array(ab);
    for (let i = 0; i < ta.length; ++i)
        ta[i] = SENTINEL(i);
    box.view = ta;            // publish to readers
    Atomics.notify(gate, "go");

    // resize-shrink / re-grow-after-shrink churn (arms 3 + 4).
    for (let step = 0; step < 10; ++step) {
        const down = MIN_LEN + ((r * 37 + step * 101) % (MAX_LEN - MIN_LEN));
        ab.resize(down);
        ab.resize(MAX_LEN);   // re-grow consumes/cancels pending tail entries
        // Re-stamp sentinels over the zero-filled re-grown tail so later
        // rounds keep the membership oracle tight (0 stays legal).
        for (let i = down; i < MAX_LEN; i += 251)
            ta[i] = SENTINEL(i);
    }

    if (r % 3 === 0) {
        // transfer = COPY + DETACH arm (source mapping enters quarantine
        // while readers may still hold {oldLen, oldBase}).
        ab.transfer(MAX_LEN >> 1);
    } else if (r % 3 === 1) {
        // plain detach via transfer() default; readers race the
        // length=0 + detached-flag publication.
        ab.transfer();
    }
    // else: drop on the floor; GC + stop retirement path.

    // Give readers a slice of the stale window before the next victim.
    sleepMs(1);
}

Atomics.store(box, "stop", 1);
Atomics.notify(gate, "go");
const counts = joinAll(readers);
for (const c of counts)
    shouldBeTrue(c > 0, "every reader must have observed live reads");
print("mc-tear-typedarray-detach-grow-shrink: PASS ("
    + counts.join(",") + " reads)");
