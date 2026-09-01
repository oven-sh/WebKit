//@ requireOptions("--useJSThreads=1")
// MC-DF S9 (docs/threads/cve/map-MC-DF.md): r269531 / bug 218944 re-opened
// under shared-everything. JSGenericTypedArrayView<Adaptor>::sort()
// (JSGenericTypedArrayViewInlines.h:950-988) copies the backing into a
// private Vector ONLY when isShared() — the SAB-era predicate
// (JSArrayBufferViewInlinesLight.h:34: FastTypedArray => false). Under
// --useJSThreads a non-SAB FastTypedArray is reachable and writable from
// any spawned thread, so the !isShared() arm runs std::sort IN PLACE on
// bytes a foreign thread is mutating. Introsort's partition loops assume
// the pivot is a sentinel; a concurrent write breaks the strict-weak
// ordering and the inner scan can run past [base, base+length) — OOB read
// AND write (the swap) into adjacent heap.
//
// Susceptibility oracle: every element written by either thread is in
// [0, N). After each sort(), every element must still be in [0, N) — any
// other value is bytes the sort read from outside the array. ASAN is the
// sharp detector (heap-buffer-overflow inside std::__sort / std::__introsort).
// The comparator path (JSGenericTypedArrayViewPrototypeFunctions.h:1775)
// always copies first and is NOT exercised here.
//
// EXECUTED POST-UNGIL ONLY. Amplifier-ready (nondeterministic interleaving;
// deterministic oracle). Trivially green under the phase-1 GIL.
load("../harness.js", "caller relative");

const N = 512;               // small enough to fit a single allocation, big
const ROUNDS = 2000;         // enough that introsort recurses several levels
const WRITERS = 2;

// Non-SAB FastTypedArray: plain `new Int32Array(N)` — no buffer materialized,
// isShared() === false.
const ta = new Int32Array(N);
for (let i = 0; i < N; ++i)
    ta[i] = i;

const gate = { started: 0, stop: 0, ta: ta };

const writers = spawnN(WRITERS, function (tid) {
    Atomics.add(gate, "started", 1);
    const t = gate.ta;
    let writes = 0;
    // Hammer pivot-adjacent indices with values that flip the < relation
    // mid-partition: alternate 0 / N-1 across the whole range.
    while (Atomics.load(gate, "stop") === 0) {
        const i = (writes * 37 + tid * 11) & (N - 1);
        t[i] = (writes & 1) ? (N - 1) : 0; // always in [0, N)
        writes++;
    }
    return writes;
});

waitUntil(() => Atomics.load(gate, "started") === WRITERS);

for (let r = 0; r < ROUNDS; ++r) {
    ta.sort(); // no comparator => JSGenericTypedArrayView::sort(), the suspect arm
    // Oracle: closed sentinel set. A value outside [0, N) was never written
    // by any thread — it came from outside the array.
    for (let i = 0; i < N; ++i) {
        const v = ta[i];
        if (v < 0 || v >= N)
            throw new Error("OOB evidence: ta[" + i + "] = " + v + " ∉ [0," + N + ") after in-place sort under race (round " + r + ")");
    }
    // Re-seed with the full index set so every round has distinct pivots.
    for (let i = 0; i < N; ++i)
        ta[i] = i;
}

Atomics.store(gate, "stop", 1);
const counts = joinAll(writers);
shouldBeTrue(counts.every(c => c > 0), "every writer made progress");
