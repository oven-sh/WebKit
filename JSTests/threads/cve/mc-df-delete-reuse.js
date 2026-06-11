//@ requireOptions("--useJSThreads=1")
// MC-DF S4 (docs/threads/cve/map-MC-DF.md): IC / structure-check-then-load
// double fetch on PROPERTY storage, sharpened to the nastiest sub-case:
// deleted-offset reuse. The fast path validates structureID (fetch 1) and
// dereferences butterfly+offset (fetch 2). If a foreign thread deletes p,
// the table edit recycles p's out-of-line offset for a NEW property q, and
// the original reader's second fetch lands after that — the reader returns
// q's value while believing it read p ("read of f returning g's value",
// SPEC-objectmodel I21). Governing invariants: I18 (no deleted out-of-line
// offset reused until an owning-heap quarantine-epoch bump postdating the
// deletion), D1/I30 (delete release-stores jsUndefined(), never clear()),
// I34 (no poll/alloc between offset fetch and access without structureID
// re-validation), M7/I24 ordering.
//
// Oracle: o.f is only ever written SENT_F and o.g only SENT_G; deletes make
// each read as undefined (D1: tardy readers see old value or undefined).
// A reader observing o.f === SENT_G (or vice versa) is offset-reuse type
// confusion = I18 violation. NaN-boxed garbage / crash = worse.
//
// EXECUTED POST-UNGIL ONLY. Amplifier-ready; GC pressure (the epoch source)
// comes from the churn allocation in the writer loop.
load("../harness.js", "caller relative");

const SENT_F = 0x0f0f0f;
const SENT_G = 0x707070;
const ROUNDS = 2000;
const READERS = 3;
const gate = { started: 0, stop: 0 };

// Push f/g out of inline storage: burn the inline capacity first.
const o = {};
for (let i = 0; i < 100; ++i)
    o["pad" + i] = i;
o.f = SENT_F;

const readers = spawnN(READERS, () => {
    Atomics.add(gate, "started", 1);
    let checks = 0;
    while (Atomics.load(gate, "stop") === 0) {
        const f = o.f;
        if (f !== SENT_F && f !== undefined)
            throw new Error("o.f read foreign slot value: " + f);
        const g = o.g;
        if (g !== SENT_G && g !== undefined)
            throw new Error("o.g read foreign slot value: " + g);
        // pad slots never move (pre-dictionary slots are stable, I18 note)
        const p = o.pad7;
        if (p !== 7)
            throw new Error("stable slot pad7 corrupted: " + p);
        checks++;
        if ((checks & 1023) === 0)
            Atomics.wait(gate, "stop", 0, 1);
    }
    return checks;
});

waitUntil(() => Atomics.load(gate, "started") === READERS);

// Writer: delete f, immediately install g (the candidate reuser of f's
// quarantined offset), delete g, reinstall f — plus allocation churn so
// quarantine epochs actually advance and promotion (takeDeletedOffset from
// Reusable only) gets exercised, not just the never-promoted easy case.
let churn = null;
for (let r = 0; r < ROUNDS; ++r) {
    delete o.f;          // D1: release-store undefined, offset -> Quarantined
    o.g = SENT_G;        // may legally reuse f's offset ONLY post-epoch (I18)
    delete o.g;
    o.f = SENT_F;
    churn = new Array(64).fill(r); // GC pressure -> epoch bumps
}
Atomics.store(gate, "stop", 1);
Atomics.notify(gate, "stop");

const counts = joinAll(readers);
for (const c of counts)
    shouldBeTrue(c > 0, "reader made progress");
shouldBe(o.f, SENT_F);
