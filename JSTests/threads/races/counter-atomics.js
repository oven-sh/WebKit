//@ requireOptions("--useJSThreads=1")
// API-I15 at scale (GI — must hold post-GIL): N x M Atomics.add(o,"x",1)
// across N threads plus M from main yields exactly (N+1)*M — each RMW is
// one atomic step (4.5/THREAD.md:5) — and a compareExchange retry loop
// (SVZ) likewise loses no increments.
//
// Amplifier target (Tools/threads/amplify.sh; TSAN no-JIT when present,
// G15). Annex T2: rendezvous blocking is bounded; every thread is joined;
// no preemptive-GIL reliance.
load("../harness.js", "caller relative");

const N = 8;
const M = 1e5;
const CAS_K = 1000; // CAS-loop increments per thread
const o = { x: 0, cas: 0 };
const gate = { go: 0, started: 0 };

const threads = spawnN(N, () => {
    Atomics.add(gate, "started", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 100); // bounded quanta
    // plain RMW storm
    for (let i = 0; i < M; ++i)
        Atomics.add(o, "x", 1);
    // CAS retry loop (the I15 second half): increment via compareExchange
    for (let i = 0; i < CAS_K; ++i) {
        for (;;) {
            const cur = Atomics.load(o, "cas");
            if (Atomics.compareExchange(o, "cas", cur, cur + 1) === cur)
                break;
        }
    }
    return "done";
});

waitUntil(() => Atomics.load(gate, "started") === N);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

// Main contends with raw atomics of its own.
for (let i = 0; i < M; ++i)
    Atomics.add(o, "x", 1);
for (let i = 0; i < CAS_K; ++i) {
    for (;;) {
        const cur = Atomics.load(o, "cas");
        if (Atomics.compareExchange(o, "cas", cur, cur + 1) === cur)
            break;
    }
}

shouldBe(joinAll(threads).join(","), "done,done,done,done,done,done,done,done");
shouldBe(o.x, (N + 1) * M, "no lost Atomics.add increments");
shouldBe(o.cas, (N + 1) * CAS_K, "no lost CAS-loop increments");
