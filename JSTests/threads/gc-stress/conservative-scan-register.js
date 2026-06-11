//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// conservative-scan-register.js — gc-stress suite: the conservative scanner
// must see references that live ONLY in a parked spawned thread's machine
// state (registers / native stack), not in any heap slot or interpreter
// local reachable from a root.
//
// Shape of the test:
//   - The spawned thread CREATES the secret object itself, so the only
//     reference anywhere in the program is the thread's local `secret`.
//   - Before parking it folds the object through a tight arithmetic chain
//     (checksum over a 64-double payload + neighbor poison slots) so the
//     value is hot in the frame; `secret` is then used again AFTER the park,
//     which forces liveness of the reference across the Atomics.wait — the
//     reference must survive in the thread's parked frame/registers.
//   - The park is the PROPERTY-path Atomics.wait (drops the GIL while
//     parked; see harness.js), so the main thread really runs GC while the
//     thread is suspended mid-frame.
//   - Main thread forces repeated full GCs ($vm.gc()) and eden GCs plus
//     allocation pressure designed to reuse any prematurely-freed cell,
//     then publishes the wake value.
//   - The thread wakes and re-derives the checksum from the SAME object.
//     If the conservative scan missed the parked thread's stack/registers,
//     the cell was swept; under --scribbleFreeCells=1 / --useZombieMode=1
//     (gc-stress-matrix.sh modes) the payload reads back scribble
//     (0xbadbeef0-family) and the checksum or tag assert trips loudly;
//     without scribbling the reuse pressure below makes a silent survive
//     unlikely.
//
// Runtime: bounded — one thread, 8 GC rounds, waits capped at 30s/60s.

load("../harness.js", "caller relative");

const PAYLOAD_LEN = 64;
const POISON = 0xdead;

// Expected checksum, computed independently (pure arithmetic, no heap).
let EXPECTED = 0;
for (let i = 0; i < PAYLOAD_LEN; ++i)
    EXPECTED += (i * 2654435761) % 1000003;

const mailbox = { threadParkedSoon: 0, gcDone: 0 };

const t = new Thread(() => {
    // The ONLY reference to this object is this local. Nothing in the
    // mailbox or any shared structure ever points at it.
    const secret = (() => {
        const o = { tagHead: POISON, tag: "alive", tagTail: POISON };
        const payload = new Array(PAYLOAD_LEN);
        for (let i = 0; i < PAYLOAD_LEN; ++i)
            payload[i] = (i * 2654435761) % 1000003;
        o.payload = payload;
        return o;
    })();

    // Tight arithmetic chain over the object BEFORE the park: keeps the
    // reference hot in the frame and pins the pre-park checksum.
    let preSum = 0;
    for (let i = 0; i < PAYLOAD_LEN; ++i)
        preSum += secret.payload[i];
    if (preSum !== EXPECTED)
        return "pre-park checksum wrong: " + preSum;

    // Tell main we are about to park, then park with the GIL dropped.
    Atomics.store(mailbox, "threadParkedSoon", 1);
    Atomics.notify(mailbox, "threadParkedSoon");
    const waitResult = Atomics.wait(mailbox, "gcDone", 0, 60000);
    if (waitResult === "timed-out")
        return "park timed out (main never finished GC rounds)";
    // Non-vacuity: the park must actually OVERLAP the GC storm. Main only
    // publishes gcDone=1 AFTER the multi-second storm (plus settle sleep),
    // so "ok" proves this thread was parked when that store landed — i.e.
    // the storm's tail ran against the parked frame. "not-equal" means
    // gcDone was already 1 when we reached the wait: the park never
    // overlapped any GC, the scan of parked-thread state went untested, and
    // reporting PASS would be silent coverage loss. With real preemption
    // (GIL-off) this thread only has to travel two statements between the
    // announce store and the wait while main burns the whole storm, so a
    // genuine "not-equal" is a scheduling pathology worth failing on, not a
    // tolerable race.
    if (waitResult !== "ok")
        return "park did not overlap GC storm: waitResult=" + waitResult;

    // Wake and USE the object: this read is what forces `secret` to be live
    // across the park. If the cell was swept while we were parked, the
    // payload/tag now read freed-cell contents (scribble under the matrix
    // modes) and the asserts below fail with the observed values.
    let postSum = 0;
    for (let i = 0; i < PAYLOAD_LEN; ++i)
        postSum += secret.payload[i];
    if (secret.tag !== "alive")
        return "object corrupted across park: tag=" + describe(secret.tag);
    if (secret.tagHead !== POISON || secret.tagTail !== POISON)
        return "object corrupted across park: poison=" + secret.tagHead + "/" + secret.tagTail;
    if (postSum !== EXPECTED)
        return "object corrupted across park: checksum " + postSum + " != " + EXPECTED;
    return "ok:" + postSum;
});

// Wait until the thread is parked (or at least past the announce store; the
// extra settle sleep lets it reach the wait itself before we GC). This is
// best-effort scheduling, NOT the overlap guarantee — that is the thread's
// own waitResult === "ok" assertion above, which fails the run if the park
// never overlapped the storm.
waitUntil(() => Atomics.load(mailbox, "threadParkedSoon") === 1, 30000);
sleepMs(50);

// GC storm + reuse pressure while the thread is parked. The transient
// objects deliberately match the secret's shapes (same property count, same
// payload array length) so a prematurely-freed cell is likely to be reused
// and rewritten — making a missed scan visible even without scribble modes.
const haveDollarVM = typeof $vm !== "undefined";
for (let round = 0; round < 8; ++round) {
    let churn = [];
    for (let i = 0; i < 2000; ++i) {
        const o = { tagHead: 0x71717171, tag: "dead", tagTail: 0x71717171 };
        o.payload = new Array(PAYLOAD_LEN).fill(0x5a5a5a5a);
        churn.push(o);
    }
    churn = null;
    if (haveDollarVM) {
        $vm.gc();
        if ($vm.edenGC)
            $vm.edenGC();
    }
}

// Wake the thread and check it still owns an intact object.
Atomics.store(mailbox, "gcDone", 1);
Atomics.notify(mailbox, "gcDone");
shouldBe(t.join(), "ok:" + EXPECTED);

print("conservative-scan-register: PASS");

// WOULD-FAIL-IF: the GC's conservative root scan does not cover a spawned
// thread's machine stack/registers while that thread is parked in the
// property-path Atomics.wait (e.g. the thread is dropped from the
// stop-the-world iteration set once it releases the GIL, or its stack
// bounds/approximate-top are recorded from the carrier rather than the
// parked frame). The secret object's only reference lives in that parked
// frame, so a missed scan sweeps the cell during the main thread's $vm.gc()
// storm; the shape-matched churn (and, in gc-stress-matrix.sh scribble /
// zombie modes, the 0xbadbeef0 scribble) rewrites the freed cell, and the
// post-wake tag/poison/checksum asserts trip deterministically. The test
// cannot pass vacuously when the park misses the storm: the thread requires
// waitResult === "ok" (parked at the moment main's post-storm gcDone store
// landed), so a run where the thread parked late (or found gcDone already
// set) reports the missed overlap as a failure instead of PASS.
