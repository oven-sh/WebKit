//@ requireOptions("--useJSThreads=1")
// SPEC-ungil ANNEX C1 / U-T10 amend, U5 lock-free-arm sentinel hardening:
// flag-on named deletes D1-store jsUndefined into the doomed slot BEFORE the
// structure publication (I30), and a delete does not touch the butterfly
// word - so a lock-free CAS/Load that validated {offset, structureID} once
// could read the quarantine sentinel through an in-flight delete (including
// a flat -> dictionary conversion followed by a dictionary delete). A
// CompareExchangeSVZ with expected === undefined would then "succeed" on an
// ABSENT property (U5), and a Load / failed CAS would surface undefined for
// a property that never held it. The amended accessors re-validate
// structureID inside the loop and disambiguate named jsUndefined reads under
// the cell lock.
//
// Owner delete/re-add storm on a flat-mode object (the repeated delete
// transitions eventually take the object to dictionary mode, so BOTH the
// flat and converted-dictionary windows are exercised) vs a foreign
// expected=undefined CAS storm + load storm on the same key. The owner only
// ever stores 7 - undefined is never a stored value - so:
//   - a CAS read of undefined = the D1 sentinel surfaced (U5 bug);
//   - a CAS read of anything other than 7 = impossible value;
//   - an applied CAS (which requires reading undefined) would leave 9
//     behind - the final value must therefore be 7;
//   - a load must yield 7 or throw TypeError (the delete won).
// Both loops are BOUNDED (phase-1 GIL is cooperative).
load("../harness.js", "caller relative");

const PER = 1200;

const o = {};
o.pad0 = 0; // A little inline padding so k lands past the first slot.
o.pad1 = 1;
o.k = 7;

const gate = { go: 0 };

const foreign = new Thread(() => {
    while (Atomics.load(gate, "go") === 0)
        sleepMs(1);
    let notEqual = 0;
    let missing = 0;
    for (let i = 0; i < PER; ++i) {
        try {
            const read = Atomics.compareExchange(o, "k", undefined, 9);
            if (read === undefined)
                throw new Error("CAS applied/observed the D1 quarantine sentinel: read undefined on a property that never held it (U5)");
            if (read !== 7)
                throw new Error("CAS read impossible value: " + String(read));
            ++notEqual;
        } catch (e) {
            if (!(e instanceof TypeError))
                throw e;
            ++missing; // The delete won: no own data property.
        }
        try {
            const loaded = Atomics.load(o, "k");
            if (loaded === undefined)
                throw new Error("Atomics.load surfaced the D1 quarantine sentinel (U5)");
            if (loaded !== 7)
                throw new Error("Atomics.load read impossible value: " + String(loaded));
        } catch (e) {
            if (!(e instanceof TypeError))
                throw e; // TypeError = the delete won; anything else is the bug.
        }
    }
    return notEqual + missing === PER;
});

Atomics.store(gate, "go", 1);
for (let i = 0; i < PER; ++i) {
    delete o.k;
    o.k = 7;
}
shouldBeTrue(foreign.join());

// No expected=undefined CAS may ever have applied: 9 must not survive.
shouldBeTrue(o.k === 7, "final value is the owner's 7 - never the CAS replacement");
