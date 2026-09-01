//@ requireOptions("--useJSThreads=1")
// semantics/proto-cycle-race.js — two threads race Object.setPrototypeOf in
// opposite directions on the same fresh pair (a -> b and b -> a). Under any
// linearization exactly one succeeds and the other MUST get the cycle-check
// TypeError; the broken outcomes are (a) both succeed, creating a prototype
// cycle (then any chain walk — getPrototypeOf loop, property miss, the
// cycle check itself — can hang or crash), or (b) the cycle check tears and
// both fail / one hangs.
//
// Per round: fresh null-prototype pair, two threads barriered to near-
// simultaneous attempts, then exact accounting plus a BOUNDED acyclicity
// walk. The whole test is bounded: fixed rounds, every thread joined.
load("../harness.js", "caller relative");

const ROUNDS = 100;

function attempt(target, proto, g, slot) {
    // Per-round 2-thread barrier so the two setPrototypeOf calls overlap.
    Atomics.add(g, "ready", 1);
    while (Atomics.load(g, "ready") < 2)
        Atomics.wait(g, "ready", 1, 2);
    try {
        Object.setPrototypeOf(target, proto);
        return "ok";
    } catch (e) {
        if (!(e instanceof TypeError || (e && e.name === "TypeError")))
            return "wrong-exception:" + e;
        return "TypeError";
    }
}

// Bounded chain walk: from o, [[Prototype]] must reach null within maxSteps.
function assertAcyclic(o, maxSteps, label) {
    let cur = o;
    for (let s = 0; s < maxSteps; ++s) {
        cur = Object.getPrototypeOf(cur);
        if (cur === null)
            return s;
    }
    throw new Error(label + ": prototype chain did not terminate within " + maxSteps + " steps (cycle!)");
}

let okTotal = 0;
for (let round = 0; round < ROUNDS; ++round) {
    const a = Object.create(null);
    const b = Object.create(null);
    a.who = "a"; b.who = "b";
    const g = { ready: 0 };

    const t1 = new Thread(attempt, a, b, g, 1); // a -> b
    const t2 = new Thread(attempt, b, a, g, 2); // b -> a
    const r1 = t1.join();
    const r2 = t2.join();

    if (r1.startsWith("wrong-exception") || r2.startsWith("wrong-exception"))
        throw new Error("round " + round + ": non-TypeError from setPrototypeOf: " + r1 + " / " + r2);

    const oks = (r1 === "ok" ? 1 : 0) + (r2 === "ok" ? 1 : 0);
    // Exactly one winner, one TypeError loser — both-succeed is the cycle
    // bug, both-fail means the check fired against a write that never
    // happened (lost update inside setPrototypeOf).
    if (oks !== 1)
        throw new Error("round " + round + ": expected exactly 1 success, got " + oks + " (" + r1 + " / " + r2 + ")");

    // Structural truth must match the reported winner.
    const aProto = Object.getPrototypeOf(a);
    const bProto = Object.getPrototypeOf(b);
    if (r1 === "ok") {
        shouldBe(aProto, b, "round " + round + ": winner a->b installed");
        shouldBe(bProto, null, "round " + round + ": loser left b's proto null");
        shouldBe(a.who, "a"); // chain works: own prop
        shouldBe(Object.create(a).who, "a"); // and one-level inheritance through the new link...
    } else {
        shouldBe(bProto, a, "round " + round + ": winner b->a installed");
        shouldBe(aProto, null, "round " + round + ": loser left a's proto null");
    }
    assertAcyclic(a, 4, "round " + round + " from a");
    assertAcyclic(b, 4, "round " + round + " from b");
    // Property miss through the chain terminates (would hang on a cycle).
    shouldBe(a.noSuchProp, undefined, "round " + round + ": miss walk terminates");
    okTotal += oks;
}
shouldBe(okTotal, ROUNDS, "one winner every round");

// WOULD-FAIL-IF: setPrototypeOf's cycle check and its [[Prototype]] install
// are not atomic against a concurrent opposite-direction setPrototypeOf —
// both threads pass the check against the pre-race chains and both install,
// creating a's chain -> b -> a. The bounded assertAcyclic walk (4 steps from
// a 2-node graph) then throws instead of hanging, the `oks !== 1` accounting
// catches both-succeed AND both-fail (a torn check that kills the legitimate
// winner), and the structural shouldBe()s catch a reported winner whose
// install was actually lost. A hang inside the racing setPrototypeOf itself
// (cycle check walking a concurrently-formed cycle) is caught by the
// runner's timeout since every other operation here is bounded.
