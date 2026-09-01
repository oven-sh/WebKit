//@ requireOptions("--useJSThreads=1", "--maxJSThreads=4")
// API-I17: spawned thread ids are in [1, 0x7ffe] and unique; exceeding
// maxJSThreads live Threads => RangeError at spawn; ids are reissued only by
// the Dev-10 rebias (not yet landed), so fresh spawns get fresh ids.
//
// --maxJSThreads=4 makes the live-cap half testable cheaply. The 4 spawned
// fns are gated on a Lock held by main across the 5th-spawn attempt, so all
// 4 threads are still live when the 5th spawn is attempted in BOTH modes:
// GIL-on, main holds the GIL continuously between spawn and the first join
// so the fns cannot have run anyway; GIL-off, a spawned thread can run in
// parallel but cannot COMPLETE (return, throw, or termination — the
// transitions that unregister it and drop the live count) until it acquires
// the gate, which main releases only after the RangeError has been observed;
// throw and termination are unreachable here: the recursion guard is
// per-thread, can-block holds on spawned threads under these flags, and no
// termination is requested. No scheduling assumption remains, and the join
// afterwards bounds the test.
load("../harness.js", "caller relative");

shouldBe(Thread.current.id, 0, "main thread id is 0 (5.1)");

const ids = new Set();
// AB18-J: gate every spawned fn on a Lock held by main until after the
// 5th-spawn check below. GIL-off, a spawned thread is in one of
// {registered-but-not-yet-running, running-before-gate, parked on
// gate.hold}; all three are live states, and the liveness-dropping
// completion transitions sit behind the gate (throw and termination are
// unreachable here — see header).
const gate = new Lock();
let threads;
gate.hold(() => {
    threads = spawnN(4, i => { gate.hold(() => {}); return i; });
    for (const t of threads) {
        shouldBeTrue(Number.isInteger(t.id), "id must be an integer");
        shouldBeTrue(t.id >= 1 && t.id <= 0x7ffe, "spawned id in [1, 0x7ffe], got " + t.id);
        shouldBeFalse(ids.has(t.id), "ids must be unique");
        ids.add(t.id);
        shouldBe(t.id, t.id, "id is stable");
    }

    // 5th live thread while all 4 are pinned live by the gate: RangeError,
    // exact message (5.1 / §3 maxJSThreads).
    shouldThrow(RangeError, () => new Thread(() => 0),
        "too many live Threads (or thread-ID space exhausted)");
});

// Gate released: the 4 threads may now acquire it, return, and unregister.
// The failed spawn must not have consumed a TID or leaked a live entry:
// after joining (threads finish and unregister), spawning works again...
shouldBe(joinAll(threads).join(","), "0,1,2,3");
const t2 = new Thread(() => "again");

// ...and pre-rebias (Dev 10) the new id is FRESH — never one of the retired
// ids, and still in range.
shouldBeFalse(ids.has(t2.id), "TIDs must not be reused before the Dev-10 rebias");
shouldBeTrue(t2.id >= 1 && t2.id <= 0x7ffe);
shouldBe(t2.join(), "again");

// Repeated spawn/join cycles keep allocating monotonically fresh unique ids.
let prev = t2.id;
for (let i = 0; i < 8; ++i) {
    const t = new Thread(() => 0);
    shouldBeTrue(t.id > prev, "ids grow monotonically pre-rebias (got " + t.id + " after " + prev + ")");
    prev = t.id;
    shouldBe(t.join(), 0);
}
