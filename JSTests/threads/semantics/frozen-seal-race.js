//@ requireOptions("--useJSThreads=1")
// semantics/frozen-seal-race.js — Object.freeze racing a property add, and
// Object.seal racing a delete, on the same objects. Exactly one side wins
// per object, and either outcome must leave the object COHERENT:
//   freeze vs add:   afterwards isFrozen is true, the original properties
//                    are bit-exact, and `added` is either fully present with
//                    its exact value (add won) or fully absent (freeze won).
//                    Half-added states (name without value, value at a wrong
//                    offset, isFrozen true but still extensible, ...) are
//                    the bug.
//   seal vs delete:  afterwards isSealed is true and `doomed` is either
//                    fully present with its original value (seal won) or
//                    fully absent (delete won).
// Sloppy-mode writes lose silently; a TypeError from the strict adder is a
// legal "freeze won" outcome and is counted, not failed.
load("../harness.js", "caller relative");

const OBJS = 200;

// --- freeze vs add ---
{
    const targets = [];
    for (let i = 0; i < OBJS; ++i)
        targets.push({ base: i, other: "o" + i });

    const gate = { ready: 0, go: 0 };
    const freezer = new Thread((objs, g) => {
        Atomics.add(g, "ready", 1);
        while (Atomics.load(g, "go") === 0)
            Atomics.wait(g, "go", 0, 2);
        for (let i = 0; i < objs.length; ++i) {
            Object.freeze(objs[i]);
            if ((i & 31) === 31)
                Atomics.wait(g, "go", 1, 1); // bounded yield, shifts the interleaving
        }
        return true;
    }, targets, gate);
    const adder = new Thread((objs, g) => {
        "use strict";
        Atomics.add(g, "ready", 1);
        while (Atomics.load(g, "go") === 0)
            Atomics.wait(g, "go", 0, 2);
        let won = 0, lost = 0;
        for (let i = 0; i < objs.length; ++i) {
            try {
                objs[i].added = 7000 + i;
                ++won;
            } catch (e) {
                if (!(e instanceof TypeError || (e && e.name === "TypeError")))
                    throw new Error("adder: expected TypeError when freeze wins, got " + e);
                ++lost;
            }
            if ((i & 31) === 0)
                Atomics.wait(g, "go", 1, 1);
        }
        return won + ":" + lost;
    }, targets, gate);

    waitUntil(() => Atomics.load(gate, "ready") === 2);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go", Infinity);
    shouldBe(freezer.join(), true);
    const [won, lost] = adder.join().split(":").map(Number);
    shouldBe(won + lost, OBJS, "adder visited every object");

    let presentCount = 0;
    for (let i = 0; i < OBJS; ++i) {
        const o = targets[i];
        shouldBeTrue(Object.isFrozen(o), "targets[" + i + "] frozen at the end");
        shouldBeFalse(Object.isExtensible(o), "targets[" + i + "] non-extensible");
        shouldBe(o.base, i, "targets[" + i + "].base intact");
        shouldBe(o.other, "o" + i, "targets[" + i + "].other intact");
        const has = Object.prototype.hasOwnProperty.call(o, "added");
        if (has) {
            ++presentCount;
            shouldBe(o.added, 7000 + i, "targets[" + i + "].added coherent (add won)");
            // If the add won, freeze ran after it: the slot must now be
            // locked like everything else.
            const desc = Object.getOwnPropertyDescriptor(o, "added");
            shouldBeFalse(desc.writable, "won slot is frozen-non-writable");
            shouldBeFalse(desc.configurable, "won slot is frozen-non-configurable");
        }
    }
    // The strict adder's success count must agree exactly with what the
    // object graph says happened — a disagreement means a write was half
    // applied or a TypeError was raised yet the property landed anyway.
    shouldBe(presentCount, won, "adder's win count matches surviving properties");
}

// --- seal vs delete ---
{
    const targets = [];
    for (let i = 0; i < OBJS; ++i)
        targets.push({ keep: i, doomed: "d" + i });

    const gate = { ready: 0, go: 0 };
    const sealer = new Thread((objs, g) => {
        Atomics.add(g, "ready", 1);
        while (Atomics.load(g, "go") === 0)
            Atomics.wait(g, "go", 0, 2);
        for (let i = 0; i < objs.length; ++i) {
            Object.seal(objs[i]);
            if ((i & 31) === 15)
                Atomics.wait(g, "go", 1, 1);
        }
        return true;
    }, targets, gate);
    const deleter = new Thread((objs, g) => {
        Atomics.add(g, "ready", 1);
        while (Atomics.load(g, "go") === 0)
            Atomics.wait(g, "go", 0, 2);
        let won = 0;
        for (let i = 0; i < objs.length; ++i) {
            if (delete objs[i].doomed) // sloppy: false when seal won
                ++won;
            if ((i & 31) === 7)
                Atomics.wait(g, "go", 1, 1);
        }
        return won;
    }, targets, gate);

    waitUntil(() => Atomics.load(gate, "ready") === 2);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go", Infinity);
    shouldBe(sealer.join(), true);
    const delWins = deleter.join();

    let gone = 0;
    for (let i = 0; i < OBJS; ++i) {
        const o = targets[i];
        shouldBeTrue(Object.isSealed(o), "targets[" + i + "] sealed at the end");
        shouldBe(o.keep, i, "targets[" + i + "].keep intact");
        const has = Object.prototype.hasOwnProperty.call(o, "doomed");
        if (has)
            shouldBe(o.doomed, "d" + i, "targets[" + i + "].doomed coherent (seal won)");
        else
            ++gone;
        // Either way, post-seal a fresh add must bounce.
        o.intruder = 1;
        shouldBeFalse("intruder" in o, "post-race add bounces on targets[" + i + "]");
    }
    shouldBe(gone, delWins, "deleter's win count matches missing properties");
}

// WOULD-FAIL-IF: the integrity-level transition (freeze/seal: structure
// swap to the non-extensible/all-non-configurable shape) is not atomic with
// respect to a racing property add/delete on another thread — e.g. the add
// slips between freeze's "set flags" and "rewrite attributes" steps leaving
// a writable property on a frozen object (caught by the descriptor checks),
// a property lands name-first so the winner check sees `added` with a wrong
// value, the strict adder's TypeError accounting disagrees with the surviving
// property count (a half-applied or double-applied write), or the sealed
// object still accepts the post-race `intruder` add. Each of the 200 objects
// is independently checked for full coherence under every legal winner, so
// any torn intermediate that escapes becomes a hard assert on that object.
