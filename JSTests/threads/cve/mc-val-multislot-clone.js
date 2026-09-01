//@ requireOptions("--useJSThreads=1")
// MC-VAL susceptibility test (docs/threads/cve/map-MC-VAL.md, surface V8):
// multi-slot consumers of a once-validated Structure.
//
// Validator: fast enumeration/clone paths (Object.assign, spread, for-in,
// Object.keys, JSON.stringify) validate ONE structure (e.g.
// canPerformFastPropertyEnumeration) and then consume MANY (offset, key)
// pairs under that single validation. Consumer assumptions under N
// mutators (OM SPEC): offsets from the validated structure stay in-bounds
// of the co-ordered butterfly (M7/I24), superseded storage is never freed
// or rewritten (I7/AS-COPY), deleted out-of-line slots read as the old
// value or jsUndefined — never garbage, never another property's value
// (D1/I18 quarantine), and no poll between offset acquisition and access
// without revalidation (I34, manifest 7b audit of unowned callers).
//
// A racing FOREIGN transition+delete storm manufactures the validator/
// consumer split: the enumerator validated S_old while the writer publishes
// S_new (segmented conversion, quarantined deletes). Every value encodes
// its key, so any cross-slot confusion (copy.k !== expect(k)) is detected.
// Semantic staleness (missing newer props, undefined for deleted) is
// allowed by the SAB-staleness model; wrong VALUES are not.
//
// Amplifier-ready; green under phase-1 GIL; bounded; all threads joined.
load("../harness.js", "caller relative");

const WRITERS = 2;
// ITERS is the iteration ceiling. The pinned official lane runs Debug
// with a 120s budget; 4000 iterations measured ~191s wall on a quiet host
// (rc=0, no oracle hits), so the verify loop below is ALSO bounded by
// TIME_BUDGET_MS — half the pinned budget, leaving headroom for spawn/join
// and lane load. MIN_ITERS guarantees each of the four (i & 3) consumer
// shapes (assign/spread/JSON/for-in) runs >= 64 sweeps even on a slow host.
// Amplified/long lanes are also budget-bounded: amplification widens
// per-iteration windows (fewer sweeps, each more potent), and the MIN_ITERS
// floor preserves shape coverage; a dedicated long lane may raise
// TIME_BUDGET_MS via its own edit if full-storm coverage is ever wanted.
const ITERS = 4000;
const MIN_ITERS = 256;
const TIME_BUDGET_MS = 60000;
const nowMs = (typeof preciseTime === "function") ? () => preciseTime() * 1000 : () => Date.now();
const o = { anchor: "anchor!" };
const gate = { started: 0, stop: 0, churn: 0 };

// Writer threads (foreign relative to main, which created o): add
// uniquely-named props whose value encodes the key, then delete a rolling
// window of them (quarantine-eligible out-of-line deletes, D1).
// spawnN passes the thread index as the sole argument; o/gate are captured
// by the shared closure scope (objects really are shared, smoke.js).
const writers = spawnN(WRITERS, (id) => {
    Atomics.add(gate, "started", 1);
    let i = 0;
    while (Atomics.load(gate, "stop") === 0) {
        const k = "w" + id + "_" + (i & 255);
        o[k] = k + "!";
        if (i > 16)
            delete o["w" + id + "_" + ((i - 16) & 255)];
        ++i;
        if ((i & 63) === 0)
            Atomics.add(gate, "churn", 1);
        // Phase-1 GIL is COOPERATIVE-ONLY (SPEC-api item 9: "Phase-1 GIL
        // preemption cooperative-only (G23/G24; yields = 5.2 blocking
        // primitives only)"): a writer that never blocks never yields, so
        // a pure spin loop here starves main forever GIL-on (the original
        // shape hung). The bounded property-path wait below parks with the
        // GIL dropped (harness.js sleepMs rationale), keeping the storm
        // schedulable GIL-on while costing GIL-off only ~1ms per 256 ops.
        if ((i & 255) === 0)
            Atomics.wait(gate, "stop", 0, 1);
    }
    return i;
});

waitUntil(() => Atomics.load(gate, "started") === WRITERS);

function checkPairs(obj) {
    let bad = 0;
    for (const k in obj) {
        const v = obj[k];
        if (k === "anchor") {
            if (v !== "anchor!")
                ++bad;
            continue;
        }
        // D1: a deleted-but-quarantined slot may surface as undefined.
        if (v !== undefined && v !== k + "!")
            ++bad;
    }
    return bad;
}

let bad = 0;
const t0 = nowMs();
let itersDone = 0;
for (let i = 0; i < ITERS; ++i) {
    if (i >= MIN_ITERS && nowMs() - t0 >= TIME_BUDGET_MS)
        break;
    itersDone = i + 1;
    switch (i & 3) {
    case 0:
        bad += checkPairs(Object.assign({}, o));
        break;
    case 1:
        bad += checkPairs({ ...o });
        break;
    case 2: {
        // JSON.stringify enumerates+reads under one validation sweep.
        const back = JSON.parse(JSON.stringify(o, (k, v) => v === undefined ? null : v));
        for (const k in back) {
            const v = back[k];
            if (k === "anchor") {
                if (v !== "anchor!")
                    ++bad;
            } else if (v !== null && v !== k + "!")
                ++bad;
        }
        break;
    }
    case 3: {
        // Direct for-in over the live object (no intermediate copy).
        bad += checkPairs(o);
        break;
    }
    }
}

Atomics.store(gate, "stop", 1);
for (const t of writers)
    shouldBeTrue(t.join() > 0);
shouldBeTrue(itersDone >= MIN_ITERS);
shouldBeTrue(Atomics.load(gate, "churn") > 0);
shouldBe(bad, 0);
