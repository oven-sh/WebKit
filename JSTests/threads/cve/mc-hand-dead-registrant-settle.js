//@ requireOptions("--useJSThreads=1")
// MC-HAND susceptibility test (docs/threads/cve/map-MC-HAND.md, surface S4).
//
// Completion-handoff to a dead owner: GIL-off, every AsyncTicket is routed
// to its REGISTRANT's inbox for settlement (SPEC-ungil §E.1/E.4). When the
// registrant dies before its ticket settles, the settle must (a) happen
// exactly once (AsyncTicket m_settled CAS, ThreadManager.cpp), (b) take the
// inboxLock-arbitrated closed-arm main fallback (settleViaRegistrantRouting:
// monotonic close, decide-under-lock, act-after-drop), and (c) deliver THE
// REGISTERED PAIR's result — never another ticket's. This is the
// CVE-2025-47907 / CVE-2020-15586 shape: a completion observed by the wrong
// logical owner after the original owner went away. SPEC-api 4.6.2/I20:
// tickets are process-owned and outlive their registering thread; a dead
// thread's ticket settles per the 5.5 protocol.
//
// Probe: two REGISTRANT threads each asyncJoin their own long-running
// TARGET thread (L1 resolves with an object; L2 rejects with an object),
// publish the promises into a shared box, and exit. Main joins both
// registrants (so they are fully dead — completion sequence ran, lite torn
// down), THEN releases the targets. Each settle therefore runs against a
// dead registrant and must take the §E.4 dead=>main routing.
// Oracle (asserted in reactions, which run on run-loop turns after the
// script body): each promise settles on the correct arm with the exact
// result CELL (heap identity) of ITS OWN target — cross-pair value bleed,
// a wrong arm, a double-settle, or a never-settle (asyncTest accounting)
// is the MC-HAND hit. Repeat asyncJoin promises are distinct but settle
// identically (4.1). Deterministic ordering; green under the phase-1 GIL
// (single shared queue), signal-bearing GIL-off.
load("../harness.js", "caller relative");

asyncTestStart(1);

const gate = { go1: 0, go2: 0 };
const out = { p: null, q: null, r: null, vp: null, vq: null, vr: null, vrRejected: 0, reactions: 0 };

const L1 = new Thread(() => {
    while (Atomics.load(gate, "go1") === 0)
        sleepMs(1);
    return { who: 1 };
});
const L2 = new Thread(() => {
    while (Atomics.load(gate, "go2") === 0)
        sleepMs(1);
    throw { who: 2 };
});

// Registrants: register tickets, attach their own reactions, publish the
// promises, die. Their reactions still fire after death (I20), on the
// settling thread's queue (dead registrant => main fallback, never an
// unrelated spawned thread's queue — SD2/SD17).
const R1 = new Thread(() => {
    const p = L1.asyncJoin();
    const q = L1.asyncJoin(); // Repeat call: distinct promise, same settlement (4.1).
    p.then(v => { out.vp = v; Atomics.add(out, "reactions", 1); },
           () => { throw new Error("R1's L1 ticket rejected (wrong settlement arm)"); });
    q.then(v => { out.vq = v; Atomics.add(out, "reactions", 1); },
           () => { throw new Error("R1's repeat L1 ticket rejected (wrong settlement arm)"); });
    out.p = p;
    out.q = q;
    return "r1-registered";
});
const R2 = new Thread(() => {
    const r = L2.asyncJoin();
    r.then(() => { throw new Error("R2's L2 ticket resolved (wrong settlement arm)"); },
           e => { out.vr = e; Atomics.add(out, "vrRejected", 1); Atomics.add(out, "reactions", 1); });
    out.r = r;
    return "r2-registered";
});

// Registrants must be FULLY dead (Phase != Running observed by join; their
// completion sequences and teardown ran) before any settle can begin.
shouldBe(R1.join(), "r1-registered");
shouldBe(R2.join(), "r2-registered");
shouldBeTrue(out.p instanceof Promise);
shouldBeTrue(out.q instanceof Promise);
shouldBeTrue(out.r instanceof Promise);
shouldBeFalse(out.p === out.q);

// Only now do the targets complete: every ticket settle targets a dead
// registrant.
Atomics.store(gate, "go1", 1);
Atomics.store(gate, "go2", 1);
const result1 = L1.join();
shouldBe(result1.who, 1);
const result2 = shouldThrow(() => L2.join());
shouldBe(result2.who, 2);

// Final verifier: attached AFTER the registrants' reactions on the same
// promises, so by FIFO reaction order out.vp/vq/vr are recorded when this
// runs. Heap-identity oracle: join() returns the same result cell the
// settle delivered — any cross-pair bleed (vp/vq !== result1, vr !==
// result2) means a completion was observed by the wrong logical owner.
Promise.all([out.p, out.q]).then(([a, b]) => {
    shouldBe(a, result1);
    shouldBe(b, result1);
    shouldBe(a.who, 1);
    shouldBe(out.vp, result1);
    shouldBe(out.vq, result1);
    out.r.then(
        () => { throw new Error("L2's asyncJoin promise resolved despite the thread throwing"); },
        e => {
            shouldBe(e, result2);
            shouldBe(e.who, 2);
            shouldBe(out.vr, result2);
            shouldBe(Atomics.load(out, "vrRejected"), 1);
            // Exactly three registrant reactions ran — one per ticket,
            // exactly once each (m_settled CAS; no double-settle).
            shouldBe(Atomics.load(out, "reactions"), 3);
            asyncTestPassed();
        });
}).catch(e => {
    print("FAIL: " + e);
    throw e;
});
