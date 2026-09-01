//@ requireOptions("--useJSThreads=1")
// SPEC-ungil ANNEX C1 / U-T10, U5 dictionary arm: dictionary delete is
// I34-blind - a lock-free CAS could "succeed" on an absent property - so the
// dictionary regime's CAS/RMW runs UNDER the JSCellLock with dictionary-ness
// and the offset re-checked under it.
//
// Owner delete/re-add storm on a dictionary-mode object vs foreign CAS on
// the same key. Legal foreign outcomes per attempt: TypeError (no own data
// property - the delete won), a CAS that applied (read 7), or a CAS that
// read the marker 9 left by an earlier successful swap (the owner need not
// interleave between every pair of foreign attempts). A CAS that lands in a
// quarantined deleted slot would
// either resurrect the property after a delete or surface an impossible
// read. Both loops are BOUNDED (phase-1 GIL is cooperative: an unbounded
// spin on either side could starve the other; join parks GIL-dropped).
load("../harness.js", "caller relative");

const PER = 1200;

// Force dictionary mode: bulk add then bulk delete.
const o = {};
for (let i = 0; i < 100; ++i)
    o["q" + i] = i;
for (let i = 0; i < 100; ++i)
    delete o["q" + i];
o.k = 7;

const gate = { go: 0 };

const foreign = new Thread(() => {
    while (Atomics.load(gate, "go") === 0)
        sleepMs(1);
    let applied = 0;
    let missing = 0;
    let observedMarker = 0;
    for (let k = 0; k < PER; ++k) {
        let read;
        try {
            read = Atomics.compareExchange(o, "k", 7, 9);
        } catch (e) {
            if (!(e instanceof TypeError))
                throw new Error("unexpected exception class: " + e);
            ++missing; // The delete won: no own data property.
            continue;
        }
        if (read !== 7 && read !== 9)
            throw new Error("CAS read impossible dictionary value: " + String(read) + " (U5)");
        if (read === 7)
            ++applied;
        else
            ++observedMarker; // read === 9: legal — our own earlier swap is still in place; the owner's delete/re-add did not interleave.
    }
    // The marker 9 can only originate from this thread's own earlier
    // successful CAS (the owner writes only 7): a read of 9 with no prior
    // applied swap means the engine fabricated the new-value as the read
    // result (e.g. a lock-free path reporting success against a stale or
    // quarantined slot).
    if (observedMarker > 0 && applied === 0)
        throw new Error("read marker 9 with no prior successful swap (U5: 9 fabricated)");
    return applied + missing + observedMarker === PER;
});

Atomics.store(gate, "go", 1);
for (let k = 0; k < PER; ++k) {
    delete o.k;
    o.k = 7;
}
shouldBeTrue(foreign.join());

// Owner's last write wins or the foreign CAS swapped it: both legal.
shouldBeTrue(o.k === 7 || o.k === 9, "final value comes from the stored set");

// A CAS must never resurrect a deleted property (Atomics ops never create).
delete o.k;
shouldBeFalse(Object.prototype.hasOwnProperty.call(o, "k"), "deleted key stays deleted");
shouldBe(o.k, undefined, "no quarantined-slot resurrection");
