//@ requireOptions("--useJSThreads=1")
// dw1-sort-comparator-osr.js — deepwater LEDGER row 1 (DW-1) regression:
// sort-comparator OSR-exit wrong-pc on spawned Threads.
//
// KNOWN RED GIL-OFF until K4.II.8 lands (LEDGER §3 item 0): the upstream
// shared-sort-scratch race in DFGSpeculativeJIT::compileArraySortCompact/
// Commit (+ FTL twin) corrupts values in the pure-int32 warmup phase —
// 12/12 SEGV on 2026-06-12 Release, 0 DW-1 dumps. Discriminators: passes
// with --useDFGJIT=0, GIL-on, and flags-off. A red run here is NOT a DW-1
// (wrong-pc) regression and NOT attributable to the OSR-exit slice.
//
// Mechanism under test: the DFG ArraySortIntrinsic inlines a small
// entries.sort(comparator); when the comparator OSR-exits, the exit ramp
// (reifyInlinedCallFrames) stashes CallSiteIndex(op_call) in the recovery
// frame's argumentCountIncludingThis tag and routes the comparator's return
// through arraySortComparatorReturnTrampoline, whose slow path
// (llint_slow_path_array_sort_comparator_return) must recover pc == the
// sort's op_call. The dive saw the debug assert fire (~1/6 of W=32 bench-bc
// runs); release would dispatch to a wrong pc. GIL-off, the recovery side now
// hard-validates the contract against the per-thread stash record, so any
// recurrence stops deterministically with discriminating evidence instead of
// silently corrupting control flow.
//
// Shape (distilled from dive-logs/variant/bench-bc.js):
//  - arrays kept at length <= 16 so handleArraySort takes the inlined
//    three-phase pipeline (compact / inlined insertion sort / commit), not
//    the >16 DirectCall fallback;
//  - the comparator is a function literal at the call site so the parser
//    body-inlines it (ArraySortComparatorCall inline frame);
//  - per-thread hot loops tier the sorting function into the DFG;
//  - after warmup, the value mix fed to the comparator flips from pure
//    int32 to mixed int32/double/boxed so comparator speculation
//    (arithmetic on a, b) OSR-exits mid-sort, repeatedly;
//  - every thread re-checks each sorted result against a reference
//    insertion sort: a wrong-pc dispatch that survives produces wrong
//    results, which this catches even in builds without the validation.
//
// Pinned GIL-off flags amplify (W spawned Threads sorting concurrently);
// flag-on GIL'd and flag-off single-thread runs must also pass.

load("./harness.js", "caller relative");

const HAVE_THREADS = typeof Thread === "function";
const W = HAVE_THREADS ? 8 : 1;
const WARMUP_ROUNDS = 2000; // tier the sort site into the DFG
const EXIT_ROUNDS = 600; // post-flip rounds that should OSR-exit in the comparator

function referenceSorted(src, cmpKind) {
    const out = src.slice();
    for (let i = 1; i < out.length; ++i) {
        const v = out[i];
        let j = i - 1;
        while (j >= 0 && keyOf(out[j]) > keyOf(v)) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = v;
    }
    return out;
}

function keyOf(x) {
    return typeof x === "object" ? x.k : +x;
}

function makeEntries(seed, len, mixed) {
    const entries = [];
    let s = seed >>> 0;
    for (let i = 0; i < len; ++i) {
        s = (s * 1103515245 + 12345) >>> 0;
        const v = s % 1000;
        if (!mixed)
            entries.push(v | 0);
        else {
            // Rotate representations: int32, double, boxed-with-valueOf-free
            // object key. Doubles and cells break the comparator's warmed
            // int32 speculation => OSR exit inside the inlined comparator.
            const r = s % 3;
            if (r === 0)
                entries.push(v | 0);
            else if (r === 1)
                entries.push(v + 0.5);
            else
                entries.push({ k: v });
        }
    }
    return entries;
}

function sortOnce(entries) {
    // Function-literal comparator at the call site => parser inlines it
    // under ArraySortComparatorCall. Arithmetic on the keys gives the DFG
    // int32 speculation to exit from once the mix flips.
    entries.sort(function comparator(a, b) {
        const ka = typeof a === "object" ? a.k : a;
        const kb = typeof b === "object" ? b.k : b;
        return (ka | 0) === ka && (kb | 0) === kb ? ka - kb : keyOf(a) - keyOf(b);
    });
    return entries;
}
noInline(sortOnce);

function checkSorted(sorted, original, tag) {
    const ref = referenceSorted(original);
    shouldBe(sorted.length, ref.length, tag + " length");
    for (let i = 0; i < sorted.length; ++i) {
        if (keyOf(sorted[i]) !== keyOf(ref[i]))
            throw new Error(tag + ": mismatch at " + i + ": " + keyOf(sorted[i]) + " != " + keyOf(ref[i]));
    }
}

function workerBody(id) {
    // Warmup: pure-int32 small arrays, hot enough to tier sortOnce (and the
    // inlined comparator) into the DFG.
    for (let r = 0; r < WARMUP_ROUNDS; ++r) {
        const entries = makeEntries(id * 7919 + r, 5 + (r % 12), false);
        const original = entries.slice();
        checkSorted(sortOnce(entries), original, "warmup t" + id + " r" + r);
    }
    // Exit phase: mixed representations force comparator OSR exits mid-sort.
    for (let r = 0; r < EXIT_ROUNDS; ++r) {
        const entries = makeEntries(id * 104729 + r, 5 + (r % 12), true);
        const original = entries.slice();
        checkSorted(sortOnce(entries), original, "exit t" + id + " r" + r);
    }
    return true;
}

if (HAVE_THREADS) {
    const threads = spawnN(W - 1, (i) => workerBody(i + 1));
    workerBody(0); // main thread participates as worker 0
    joinAll(threads);
} else
    workerBody(0);
