//@ requireOptions("--useJSThreads=1")
// MC-LOCK S6 (docs/threads/cve/map-MC-LOCK.md): SUSPECTED HOLE — foreign
// blank-indexing first install (the N3 leg of
// JSObject::createInitialIndexedStorageConcurrent, JSObject.cpp) performs a
// foreign butterfly-less transition WITHOUT the F2 fire that SPEC-objectmodel
// §5 F2 / I10 key on "butterfly-less transition by a thread !=
// S->transitionThreadLocalTID()" (the blank->ArrayStorage leg DOES fire).
// While the TTL sets are valid the owner is chartered to publish structure-
// only transitions with TODAY'S PLAIN CODE (E4 / N2-(i): plain setStructure,
// no nuke, no CAS). Interleaving the owner's plain store between the foreign
// N3 leg's nuke-CAS and its final plain setStructure yields either:
//   (w1) the owner's transition silently lost (lost property add, I21), or
//   (w2) a {blank-indexing structure, installed contiguous butterfly} torn
//        pair (structure/butterfly mismatch, I21) — GC derives butterfly
//        base/extent from the STRUCTURE, so (w2) mis-sizes the marker's scan.
// This is the JEP-374 biased-locking lesson: a revocation (F2 fire) skipped
// on one trigger path leaves the bias owner racing the revoker's multi-step
// publication.
//
// Oracle (I21): on every round BOTH racing writes must survive — the owner's
// inline property add AND the foreign first indexed install are on disjoint
// slots, so JS semantics admit no lost update. A missing property, missing
// element, or crash is a hit. (w2) may also surface later as a GC crash under
// the allocation churn below.
//
// EXECUTED POST-UNGIL ONLY (phase-1 GIL fully masks the window).
// Amplifier-ready: the high-value hook points are the N3 nuke-CAS and the E4
// owner setStructure publication.
load("../harness.js", "caller relative");

const ROUNDS = 5000;
const FOREIGN_SENT = 0x5e117;
const gate = { round: 0, fdone: 0, stop: 0 };
const channel = { obj: null };

const foreign = new Thread(() => {
    let seen = 0;
    while (Atomics.load(gate, "stop") === 0) {
        const r = Atomics.load(gate, "round");
        if (r === seen) {
            Atomics.wait(gate, "round", seen, 1);
            continue;
        }
        seen = r;
        const o = channel.obj;
        // FOREIGN first indexed install on a blank-indexing, butterfly-less
        // object: word == 0 => the N3 leg (nuke-CAS, casButterfly(0->...),
        // plain setStructure) with NO F2 fire — racing the owner's E4 add.
        o[0] = FOREIGN_SENT;
        Atomics.store(gate, "fdone", seen);
        Atomics.notify(gate, "fdone");
    }
    return seen;
});

let churn = null;
for (let r = 1; r <= ROUNDS; ++r) {
    // Fresh structure chain every round: a unique leading property name keeps
    // this round's TTL sets valid (monotone sets — once fired the E4 window
    // closes for that chain forever), so the owner's add below stays on the
    // E4 plain-store path even if a previous round's race fired something.
    const o = {};
    o["shape" + r] = r;
    channel.obj = o;
    Atomics.store(gate, "round", r);
    Atomics.notify(gate, "round");

    // OWNER inline add: structure-only transition, butterfly untouched —
    // E4/N2-(i) "today's code": plain setStructure while the sets are valid.
    o.b = r;

    while (Atomics.load(gate, "fdone") !== r)
        Atomics.wait(gate, "fdone", Atomics.load(gate, "fdone"), 1);

    // --- I21 oracle: both disjoint writes survived. ---
    if (o.b !== r)
        throw new Error("round " + r + ": owner inline add lost (w1: foreign N3 "
            + "final setStructure clobbered the owner's transition): o.b = " + o.b);
    if (o[0] !== FOREIGN_SENT)
        throw new Error("round " + r + ": foreign first install lost: o[0] = " + o[0]);
    if (o["shape" + r] !== r)
        throw new Error("round " + r + ": pre-race property corrupted: " + o["shape" + r]);
    // Structure/butterfly coherence probe for (w2): an indexed read through a
    // blank-indexing structure with an installed butterfly, and enumeration,
    // both walk the pair; churn gives the GC chances to scan the torn pair.
    if (Object.keys(o).length !== 2 + 1) // shapeN, b + index "0"
        throw new Error("round " + r + ": key set wrong (torn structure?): " + Object.keys(o));
    churn = new Array(32).fill(r); // GC pressure: (w2) also surfaces as a marking crash
}

Atomics.store(gate, "stop", 1);
Atomics.store(gate, "round", ROUNDS + 1);
Atomics.notify(gate, "round");
shouldBe(foreign.join(), ROUNDS);
