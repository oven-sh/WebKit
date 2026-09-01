//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-TDWN S10 (docs/threads/cve/map-MC-TDWN.md): TID retire/reissue vs a
// dead thread's residual tagged state — the sync.Pool/ETS
// reuse-after-teardown analog, and the chartered U-T12 verification arm (1)
// shape (SPEC-ungil §D.1 / ANNEXES D1+D1R; ThreadManager.h rebias banner).
//
// GIL-OFF ONLY (gilOffProcess): GIL-on, retired TIDs are never recycled
// (Deviation 10) and this test would exhaust permanently by design.
//
// Storm: spawn/join far past the 75% consumption trigger of the spawned
// TID range [1, 0x4000) (~12288), driving retire -> seal -> full-stop
// restamp+jettison -> reissue. Dead threads leave behind:
//   - objects whose butterflies were stamped with their (now-dead) TIDs,
//   - structures whose transition-TLS TID is a dead TID,
// and the test then makes FRESH threads (holding reissued TIDs) read,
// extend, and transition exactly those objects. If reissue ever precedes
// the in-stop restamp + D1R watchpoint fires, a fresh thread aliases the
// dead thread's thread-local fast paths: observable as wrong values,
// spurious ConcurrentAccessError, or a crash.
//
// Recovery: SD9 — exhaustion surfaces as RangeError("too many live
// Threads (or thread-ID space exhausted)"); the spawn host call requests
// a full collection when a Sealed snapshot is pending, so the gate must
// LIFT within bounded retries (no organic allocation pressure needed).
//
// Deterministic in outcome, storm-shaped in schedule; slow (≈17k OS
// thread spawn/joins). Amplifier-ready: RaceAmplifier stall points sit on
// retireCarrierTID / conductTIDRebiasUnderSharedStop.
load("../harness.js", "caller relative");

const SPAWN_TARGET = 17000; // > 16383-TID range: guarantees crossing exhaustion or recycling
const BATCH = 32;

const keepsakes = []; // dead threads' tagged objects, one per ~256 spawns

function spawnBatch(base) {
    const threads = [];
    for (let i = 0; i < BATCH; ++i) {
        const keep = ((base + i) % 256) === 0;
        threads.push(new Thread((n, wantKeepsake) => {
            // Per-thread structure transitions + butterfly growth: this
            // thread's TID lands in transition-TLS state and object tags.
            const o = {};
            o["p" + (n % 7)] = n;
            o.a = n; o.b = n + 1; o.c = n + 2;
            o[0] = n; o[1] = n + 1; // indexed butterfly too
            if (wantKeepsake)
                return { obj: o, n };
            return n;
        }, base + i, keep));
    }
    for (let i = 0; i < BATCH; ++i) {
        const r = threads[i].join();
        if (typeof r === "object") {
            shouldBe(r.obj.a, base + i, "dead-thread object readable by parent");
            keepsakes.push(r);
        } else
            shouldBe(r, base + i, "thread result intact");
    }
}

let spawned = 0;
let sawExhaustion = false;
while (spawned < SPAWN_TARGET) {
    try {
        spawnBatch(spawned);
        spawned += BATCH;
    } catch (e) {
        // SD9 exhaustion gate. Must be the api 5.1 RangeError, nothing else.
        if (!(e instanceof RangeError))
            throw e;
        sawExhaustion = true;
        // Recovery: every spawned thread above is already joined (dead =>
        // retired). Retry with bounded patience: the VM-aware spawn
        // overload requests the full collection that runs the restamp;
        // the gate must lift without external allocation pressure.
        let recovered = false;
        for (let attempt = 0; attempt < 200 && !recovered; ++attempt) {
            sleepMs(10);
            try {
                const probe = new Thread(() => 42);
                shouldBe(probe.join(), 42);
                recovered = true;
            } catch (e2) {
                if (!(e2 instanceof RangeError))
                    throw e2;
            }
        }
        shouldBeTrue(recovered, "SD9 gate lifted after rebias (TID reissue recovered)");
        spawned += 1; // the probe
    }
}

// Post-recycle cross-check: FRESH threads (reissued TIDs) attack the DEAD
// threads' residual tagged state. Any un-restamped dead TID aliasing a
// reissued one shows up here as a stale thread-local fast path: wrong
// reads, spurious ConcurrentAccessError, or worse.
shouldBeTrue(keepsakes.length > 0);
const verifiers = [];
for (let v = 0; v < 8; ++v) {
    verifiers.push(new Thread((keeps, salt) => {
        let sum = 0;
        for (const k of keeps) {
            if (k.obj.a !== k.n || k.obj[0] !== k.n)
                throw new Error("stale value through recycled-TID fast path at n=" + k.n);
            k.obj["fresh" + salt] = salt;       // foreign transition on a dead thread's structure
            sum += k.obj.b - k.obj.a;           // foreign butterfly reads
        }
        return sum;
    }, keepsakes, v));
}
for (let v = 0; v < 8; ++v)
    shouldBe(verifiers[v].join(), keepsakes.length, "verifier " + v + " saw consistent dead-thread objects");
for (const k of keepsakes) {
    for (let v = 0; v < 8; ++v)
        shouldBe(k.obj["fresh" + v], v, "foreign transitions on dead-thread structures all landed");
}

// Note: sawExhaustion may legitimately be false if continuous recycling
// (the post-75% regime) keeps reissue ahead of consumption — that is the
// D1 success mode, not a test failure.
