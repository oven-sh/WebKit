//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel §6 delete quarantine semantics (I18, I30/D1).
//
// Deleted out-of-line slots are quarantined until a GC-epoch bump, so a
// racing stale reader of the deleted property can never alias a NEWLY ADDED
// property's value through slot reuse. Observable invariant: while one
// thread deletes o.victim and then adds fresh properties (which, without
// quarantine, could be handed the victim's slot), any read of o.victim
// yields either its pre-delete value or undefined — never a new property's
// value. D1 additionally pins the tardy-reader outcome to old-value-or-
// undefined (the slot is release-stored undefined, never left stale).
load("../resources/assert.js", "caller relative");

const ROUNDS = 8;
const PAD = 40;          // force out-of-line storage
const NEW_PROPS = 40;    // enough adds to reuse any prematurely freed slot
const SAMPLES = 800;

for (let round = 0; round < ROUNDS; ++round) {
    const o = {};
    for (let i = 0; i < PAD; ++i)
        o["p" + i] = "old" + i;

    const reader = new Thread(() => {
        for (let s = 0; s < SAMPLES; ++s) {
            const v = o.p7;
            if (v !== "old7" && v !== undefined)
                throw new Error("round " + round + ": deleted p7 aliased "
                    + "another value through slot reuse: " + describe(v));
            // Surviving neighbors must be untouched at all times.
            const n = o.p8;
            if (n !== "old8")
                throw new Error("round " + round + ": neighbor p8 corrupted by "
                    + "delete: " + describe(n));
        }
        return true;
    });
    const mutator = new Thread(() => {
        delete o.p7;
        for (let i = 0; i < NEW_PROPS; ++i)
            o["q" + i] = "new" + i;
    });

    shouldBeTrue(reader.join());
    mutator.join();

    // Final-state checks (deterministic regardless of interleaving).
    shouldBeFalse("p7" in o, "round " + round + ": delete lost");
    shouldBe(o.p7, undefined);
    for (let i = 0; i < PAD; ++i) {
        if (i === 7)
            continue;
        shouldBe(o["p" + i], "old" + i, "round " + round + ": survivor p" + i);
    }
    for (let i = 0; i < NEW_PROPS; ++i)
        shouldBe(o["q" + i], "new" + i, "round " + round + ": post-delete add q" + i);
    shouldBe(Object.keys(o).length, PAD - 1 + NEW_PROPS);
}

// Delete + re-add of the SAME name racing readers: a read of o.r must be the
// old value, undefined (mid delete/re-add window), or the new value — never
// any other property's value.
for (let round = 0; round < ROUNDS; ++round) {
    const o = {};
    for (let i = 0; i < PAD; ++i)
        o["f" + i] = i; // numbers, so string sentinels below are unambiguous
    o.r = "before";

    const reader = new Thread(() => {
        for (let s = 0; s < SAMPLES; ++s) {
            const v = o.r;
            if (v !== "before" && v !== "after" && v !== undefined)
                throw new Error("round " + round + ": o.r read foreign value "
                    + describe(v));
        }
        return true;
    });
    const mutator = new Thread(() => {
        delete o.r;
        for (let i = 0; i < 16; ++i)
            o["filler" + i] = i; // tempt slot reuse before the re-add
        o.r = "after";
    });

    shouldBeTrue(reader.join());
    mutator.join();

    shouldBe(o.r, "after", "round " + round + ": re-add lost");
    for (let i = 0; i < PAD; ++i)
        shouldBe(o["f" + i], i);
    for (let i = 0; i < 16; ++i)
        shouldBe(o["filler" + i], i);
}

// "in" / read agreement after the mutator is fully joined: a name reported
// absent must read undefined; a name reported present must read its value.
{
    const o = {};
    for (let i = 0; i < PAD; ++i)
        o["d" + i] = "val" + i;
    const deleter = new Thread(() => {
        for (let i = 0; i < PAD; i += 2)
            delete o["d" + i];
    });
    deleter.join();
    for (let i = 0; i < PAD; ++i) {
        const name = "d" + i;
        if (i % 2 === 0) {
            shouldBeFalse(name in o, name + " should be deleted");
            shouldBe(o[name], undefined);
        } else {
            shouldBeTrue(name in o, name + " should survive");
            shouldBe(o[name], "val" + i);
        }
    }
    shouldBe(Object.keys(o).length, PAD / 2);
}
