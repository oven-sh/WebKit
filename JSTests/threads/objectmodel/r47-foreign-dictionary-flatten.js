//@ requireOptions("--useJSThreads=1")
// r47 manifest-7 audit escape #2/#3 regression (SCALEBENCH §47, FUZZ.md r47):
// dictionary flatten on a FOREIGN thread shifts/restamps the butterfly via
// JSObject::setButterfly -> storeTaggedButterflyWordConcurrent, whose
// owner-TID RELEASE_ASSERT (JSObjectInlines.h:116) traps when the object's
// butterfly was tagged by another thread. Flag-on flatten already runs
// world-stopped + cell-locked (F3 / §6 L3); the publication form must be the
// tag-preserving cell-locked store, not the owner-only one.
//
// Covers BOTH publication sites:
//   - shiftButterflyAfterFlattening (JSObject.cpp): out-of-line capacity
//     shrinks but stays > 0 -> fresh shifted butterfly, tag-preserved.
//   - flattenDictionaryStructureImpl (Structure.cpp): out-of-line capacity
//     drops to 0 with no indexing header -> butterfly word nulled.
//
// Uses $vm.flattenDictionaryObject as the deterministic foreign trigger when
// the corpus runner provides --useDollarVM=1 (run-tests.sh does); otherwise
// falls back to a delete-IC churn that reaches the unguarded
// tryCacheDeleteBy flatten on a foreign thread probabilistically.
load("../harness.js", "caller relative");

const haveDollarVM = typeof $vm !== "undefined" && typeof $vm.flattenDictionaryObject === "function";

const ROUNDS = haveDollarVM ? 8 : 32;
// Enough out-of-line storage that a partial delete shrinks the capacity
// bucket (outOfLineCapacity halves), forcing the shift path.
const PAD = 40;

function makeUncacheableDictionary(o, deleteFrom, deleteCount) {
    // Two deletes already promote to UncacheableDictionary; do a few more so
    // the post-compaction outOfLineSize < pre-flatten outOfLineCapacity.
    for (let i = 0; i < deleteCount; ++i)
        delete o["p" + (deleteFrom + i)];
    // Re-add a couple so renumberPropertyOffsets has work to do.
    o["q0"] = "q0";
    o["q1"] = "q1";
    delete o["q0"];
}

function flattenOnForeignThread(o, expectShift) {
    const t = new Thread(() => {
        if (haveDollarVM) {
            $vm.flattenDictionaryObject(o);
        } else {
            // Probabilistic fallback: tryCacheDeleteBy (Repatch.cpp) flattens
            // a dictionary base before caching the delete IC, with no gilOff
            // gate. Churn until either it fires or the budget runs out.
            for (let i = 0; i < 200; ++i) {
                o["churn" + i] = i;
                delete o["churn" + i];
            }
        }
        // Read survivors back so a torn/relocated slot would be observed.
        if (expectShift) {
            if (o.p0 !== "v0")
                throw new Error("post-flatten p0 = " + o.p0);
            if (o.q1 !== "q1")
                throw new Error("post-flatten q1 = " + o.q1);
        } else {
            if (o.p0 !== undefined)
                throw new Error("post-flatten (null case) p0 = " + o.p0);
        }
    });
    t.join();
}

for (let r = 0; r < ROUNDS; ++r) {
    // ----- shift case: capacity shrinks but stays > 0 -----
    {
        const o = {};
        for (let i = 0; i < PAD; ++i)
            o["p" + i] = "v" + i;
        // Delete the upper half: post-compaction out-of-line size ~= PAD/2,
        // so afterOutOfLineCapacity < beforeOutOfLineCapacity (shift path).
        makeUncacheableDictionary(o, PAD / 2, PAD / 2);
        flattenOnForeignThread(o, /* expectShift */ true);
        // Main-thread re-validation (the foreign flatten preserved the tag,
        // so the original owner's reads must still hit the right slots).
        shouldBe(o.p0, "v0");
        shouldBe(o["p" + (PAD / 2 - 1)], "v" + (PAD / 2 - 1));
        shouldBe(o.q1, "q1");
        shouldBe(o["p" + (PAD - 1)], undefined);
    }

    // ----- null case: every out-of-line property deleted, no indexing header -----
    {
        const o = {};
        for (let i = 0; i < PAD; ++i)
            o["p" + i] = "v" + i;
        for (let i = 0; i < PAD; ++i)
            delete o["p" + i];
        // Now an UncacheableDictionary with 0 live out-of-line properties and
        // no indexing header: flatten will setButterfly(nullptr).
        flattenOnForeignThread(o, /* expectShift */ false);
        shouldBe(o.p0, undefined);
        // The object stays usable after the foreign null-out.
        o.after = 1;
        shouldBe(o.after, 1);
    }
}

// ----- the symmetric direction: object CREATED on a worker, FLATTENED on main -----
// (the trap is butterflyTID(old) != currentButterflyTID(); either side suffices,
// this covers the tag preservation when the original installer reads back).
{
    let o;
    new Thread(() => {
        o = {};
        for (let i = 0; i < PAD; ++i)
            o["p" + i] = "w" + i;
        for (let i = PAD / 2; i < PAD; ++i)
            delete o["p" + i];
    }).join();
    if (haveDollarVM)
        $vm.flattenDictionaryObject(o);
    shouldBe(o.p0, "w0");
    shouldBe(o["p" + (PAD / 2 - 1)], "w" + (PAD / 2 - 1));
    shouldBe(o["p" + (PAD - 1)], undefined);
}
