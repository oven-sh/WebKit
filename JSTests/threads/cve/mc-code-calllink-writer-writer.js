//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// MC-CODE S7 (docs/threads/cve/map-MC-CODE.md): concurrent slow-path call
// linking — GIL-removal precondition 11 (INTEGRATE-jit.md; caveat at
// bytecode/CallLinkInfo.cpp publishRecord). CallLinkInfo::publishRecord uses
// a NON-ATOMIC std::exchange on the plain m_record and the slow-path linkers
// (linkMonomorphicCall / setVirtualCall / setStub / linkDirectCall,
// bytecode/Repatch.cpp) take no lock. Under N mutators, two threads taking
// the SAME unlinked call site's slow path can both observe the SAME
// oldRecord and retire it TWICE => double-delete at epoch expiry (heap
// corruption), plus torn m_callee/m_codeBlock/m_mode mirror writes.
//
// This differs from jit/int-gate-direct-call-relink.js: that gate stresses
// READERS against a single conductor relinking. Here we isolate the
// WRITER-WRITER window: per round, a FRESH CodeBlock with one unlinked call
// site is published, and all workers rendezvous and make their FIRST calls
// through it simultaneously — each with a DIFFERENT callee, so the racing
// slow paths are linkMonomorphicCall (different comparands), then the
// immediate misses force setVirtualCall/setStub republishes on the same
// CallLinkInfo. Periodic $vm.gc() drives epoch expiry, where a double-retire
// becomes a double-delete.
//
// Oracle: every call must return calleeId-consistent values (a torn
// comparand/target pair mismatches), and no crash. The double-delete itself
// is best surfaced under ASAN — run this file in the ASAN ladder rung.
// EXECUTED POST-UNGIL ONLY (under the phase-1 GIL the slow paths serialize
// and the window cannot open). Amplifier-ready: the rendezvous bounds the
// window to the first-call instant; the amplifier widens it.
load("../harness.js", "caller relative");

const WORKERS = 4;
const ROUNDS = 300;
const CALLS_PER_ROUND = 24;

const gate = { round: 0, done: 0, ready: 0 };
// Published per round: a fresh call-site function + per-worker callees.
const shared = { site: null, callees: null };

function makeCallee(id) {
    return Function("x", "return x * 1000 + " + id + ";");
}

const workers = spawnN(WORKERS, (tid) => {
    Atomics.add(gate, "ready", 1);
    let calls = 0;
    for (let r = 1; r <= ROUNDS; ++r) {
        // Rendezvous: wait for round r's fresh site to be published.
        while (Atomics.load(gate, "round") < r)
            Atomics.wait(gate, "round", r - 1, 1);
        const site = shared.site;
        const mine = shared.callees[tid];
        const others = shared.callees;
        // First call: this thread's linkMonomorphicCall races every other
        // worker's on the SAME CallLinkInfo.
        for (let i = 0; i < CALLS_PER_ROUND; ++i) {
            // Rotate callees so the site is immediately polymorphic =>
            // upgrade/virtual/stub republishes keep hammering m_record.
            const c = others[(tid + i) % WORKERS];
            const expectId = (tid + i) % WORKERS;
            const got = site(c, i);
            if (got !== i * 1000 + expectId)
                throw new Error("round " + r + " worker " + tid + ": call returned " + got + ", expected " + (i * 1000 + expectId) + " — torn call-link record");
            ++calls;
        }
        Atomics.add(gate, "done", 1);
    }
    return calls;
});

waitUntil(() => Atomics.load(gate, "ready") === WORKERS);

for (let r = 1; r <= ROUNDS; ++r) {
    // Fresh executable => fresh CodeBlock => fresh, UNLINKED CallLinkInfo at
    // the `c(x)` site. Fresh callees too, so prior rounds' links can go weak
    // and unlink under GC (single-null-store path) while retired records sit
    // in the epoch.
    shared.callees = [];
    for (let w = 0; w < WORKERS; ++w)
        shared.callees.push(makeCallee(w));
    shared.site = Function("c", "x", "return c(x);");
    Atomics.store(gate, "round", r);
    Atomics.notify(gate, "round");
    waitUntil(() => Atomics.load(gate, "done") === WORKERS * r);
    if (r % 25 === 0)
        $vm.gc(); // epoch expiry: a double-retired record double-deletes here (ASAN)
}

const counts = joinAll(workers);
for (const c of counts)
    shouldBe(c, ROUNDS * CALLS_PER_ROUND);
$vm.gc();
print("mc-code-calllink-writer-writer: PASS (" + WORKERS + " workers x " + ROUNDS + " rounds)");
