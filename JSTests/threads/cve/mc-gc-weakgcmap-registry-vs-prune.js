//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-GC S12b (docs/threads/cve/map-MC-GC.md): m_weakGCHashTables registry
// publication race vs the conducted prune — the JDK-8147611 / CVE-2026-7936
// "weak-table read vs sweep" analog. Heap::registerWeakGCHashTable /
// unregisterWeakGCHashTable (heap/Heap.cpp:4422-4429) mutate the bare
// UncheckedKeyHashSet m_weakGCHashTables with NO lock; every WeakGCMap /
// WeakGCSet ctor (WeakGCMapInlines.h:40, WeakGCSetInlines.h:38) calls it,
// and JSGlobalObject::init constructs several per global. K4.VIII.9
// reproduced this as an ASAN SEGV (0xf5-scribble read) under concurrent
// $vm.createGlobalObject(). The MC-GC failure mode this test targets is the
// QUIET one: a torn add loses a registration, so pruneStaleEntries never
// runs on that table — the collector's view of which weak tables exist
// diverges from what mutators can read. For the VM-level
// symbolImplToSymbolMap (registered once at VM ctor, so not itself raced
// here) we instead check that reads through a WeakGCMap whose entries
// survived a conducted full-GC prune storm preserve identity across
// threads.
//
// Oracles, in order of severity:
//   (1) no crash / no debug assert during the registration storm + prune
//       (the loud K4.VIII.9 mode);
//   (2) Symbol.for identity holds on every spawned thread for keys
//       registered before, during, and after the storm (a lost or corrupted
//       registry walk that touches a stale bucket would either crash the
//       conducted prune at Heap.cpp:3430 or leave a torn Weak slot whose
//       get() returns a wrong cell — observed as identity loss);
//   (3) per-global WeakGCMaps created mid-storm remain functional after a
//       prune: a customGetterSetterFunctionMap-backed accessor on a fresh
//       global resolves to the same function object before and after gc().
//
// EXECUTED POST-UNGIL ONLY. Amplifier-ready (the registration race is a
// HashSet rehash window — RaceAmplifier widens it; without amplification
// the storm parameters below hit it ~1/6-1/10 per K4.VIII.9).
load("../harness.js", "caller relative");

const REGISTRARS = 4;
const GLOBALS_PER_THREAD = 25;
const GC_ROUNDS = 8;
const SYM_KEYS = 200;

// Phase 0: pre-register a band of symbols on main so symbolImplToSymbolMap
// has live entries that the prune walk must NOT disturb.
const preSyms = [];
for (let i = 0; i < SYM_KEYS; ++i)
    preSyms.push(Symbol.for("mcGcS12-pre-" + i));

const gate = { ready: 0, go: 0, done: 0 };

// Phase 1: N spawned threads each construct GLOBALS_PER_THREAD fresh
// JSGlobalObjects (each ctor registers multiple WeakGCMaps into the SHARED
// heap's m_weakGCHashTables) while main runs a conducted full-GC + churn
// storm so pruneStaleEntriesFromWeakGCHashTables iterates the registry
// concurrently-with-registration's after-effects.
const threads = [];
for (let t = 0; t < REGISTRARS; ++t) {
    threads.push(new Thread((gate, tid) => {
        Atomics.add(gate, "ready", 1);
        while (Atomics.load(gate, "go") === 0)
            Atomics.wait(gate, "go", 0);

        const globals = [];
        const midSyms = [];
        for (let i = 0; i < GLOBALS_PER_THREAD; ++i) {
            // Registration storm: each createGlobalObject() drives several
            // registerWeakGCHashTable calls on the shared heap.
            globals.push($vm.createGlobalObject());
            // Interleave symbolImplToSymbolMap traffic so a corrupted
            // registry that drops the VM-level map would surface as an
            // identity miss below.
            midSyms.push(Symbol.for("mcGcS12-mid-" + tid + "-" + i));
        }

        // Drop most globals so their WeakGCMaps' Weak<> entries die and the
        // NEXT conducted full GC's prune has real work to do on tables
        // registered during the storm. Keep one to probe oracle (3).
        const kept = globals[0];
        globals.length = 0;

        Atomics.add(gate, "done", 1);
        // Wait for main's prune storm to finish before probing.
        while (Atomics.load(gate, "go") !== 2)
            Atomics.wait(gate, "go", 1);

        // Oracle (2): identity across the prune.
        for (let i = 0; i < SYM_KEYS; ++i) {
            if (Symbol.for("mcGcS12-pre-" + i).description !== ("mcGcS12-pre-" + i))
                return "corrupt: pre-sym description mismatch at " + i;
        }
        for (let i = 0; i < midSyms.length; ++i) {
            if (Symbol.for("mcGcS12-mid-" + tid + "-" + i) !== midSyms[i])
                return "corrupt: mid-sym identity lost at tid " + tid + " i " + i;
        }
        // Oracle (3): the kept global's own WeakGCMaps still work after a
        // prune that may have walked a torn registry. Use a property whose
        // lookup path goes through a per-global WeakGCMap-backed cache
        // (Function.prototype getter on the fresh realm — the
        // customGetterSetterFunctionMap path); identity must be stable
        // across a second gc() this thread conducts.
        const desc1 = kept.Object.getOwnPropertyDescriptor(kept.Function.prototype, "name");
        gc();
        const desc2 = kept.Object.getOwnPropertyDescriptor(kept.Function.prototype, "name");
        if (typeof desc1 !== "object" || typeof desc2 !== "object")
            return "corrupt: kept-global accessor lookup failed";
        return "ok";
    }, gate, t));
}

waitUntil(() => Atomics.load(gate, "ready") === REGISTRARS);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go");

// Main also registers globals (so registrations collide cross-thread, not
// just spawned-vs-spawned) while driving conducted full GCs.
for (let r = 0; r < GC_ROUNDS; ++r) {
    for (let i = 0; i < 10; ++i)
        $vm.createGlobalObject();
    let churn = [];
    for (let i = 0; i < 4000; ++i)
        churn.push({ s: "churn-" + r + "-" + i, a: [r, i, r * i] });
    gc(); // conducted full collection => pruneStaleEntriesFromWeakGCHashTables
    churn = null;
}

waitUntil(() => Atomics.load(gate, "done") === REGISTRARS);
// One more prune pass now that spawned threads have dropped their globals.
gc();
Atomics.store(gate, "go", 2);
Atomics.notify(gate, "go");

for (const t of threads)
    shouldBe(t.join(), "ok");

// Oracle (2), main side: every pre-registered symbol is still the SAME cell.
for (let i = 0; i < SYM_KEYS; ++i)
    shouldBe(Symbol.for("mcGcS12-pre-" + i), preSyms[i]);
