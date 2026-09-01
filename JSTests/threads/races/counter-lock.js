//@ requireOptions("--useJSThreads=1")
// API-I6 at scale (GI — must hold post-GIL): N=8 threads x M=1e5
// lock.hold(() => counter.n++) on a shared property, the main thread
// contending with M holds of its own, produces exactly (N+1)*M; the gate
// construction guarantees >=2 waiters parked on the lock at once.
//
// Amplifier target: run under Tools/threads/amplify.sh (+ TSAN no-JIT when
// present, G15). Annex T2: no preemptive-GIL reliance — every interleaving
// point is a blocking primitive of the program itself; all blocking ops are
// bounded (gate rendezvous has a deadline, every fn terminates, every
// thread is joined).
load("../harness.js", "caller relative");

const N = 8;
const M = 1e5;
const lock = new Lock();
const counter = { n: 0 };
const gate = { go: 0, started: 0 };

const threads = spawnN(N, () => {
    Atomics.add(gate, "started", 1);
    // Rendezvous: park until main opens the gate (bounded quanta — a missed
    // notify costs at most 100ms).
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 100);
    for (let i = 0; i < M; ++i)
        lock.hold(() => { counter.n++; });
    return "done";
});

// Open the gate while HOLDING the lock: every thread's first hold() then
// contends against main's hold and parks in m_lock — with N=8 released
// together, >=2 waiters are parked at once by construction.
lock.hold(() => {
    waitUntil(() => Atomics.load(gate, "started") === N);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go", Infinity);
    // Give the woken threads time to reach their first (contended) hold
    // before we release: they park on m_lock while we still hold it.
    sleepMs(20);
});

// Main thread contends with its own M holds.
for (let i = 0; i < M; ++i)
    lock.hold(() => { counter.n++; });

shouldBe(joinAll(threads).join(","), "done,done,done,done,done,done,done,done");
shouldBe(counter.n, (N + 1) * M, "no lost increments under contended lock.hold");
shouldBeFalse(lock.locked);
