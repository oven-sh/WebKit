//@ requireOptions("--useJSThreads=1")
// MC-TEAR S5 (docs/threads/cve/map-MC-TEAR.md): racing JSRopeString
// resolution. UNGIL-HANDOUT §N.2 rules resolution lock-free with publication
// by ONE release-CAS of the fiber0/flags word (losers discard, readers
// load-acquire). At audit time convertToNonRope
// (Source/JavaScriptCore/runtime/JSStringInlines.h:382-393) is still a plain
// placement-new + storeStoreFence — no CAS, no loser arm — so N GIL-off
// threads resolving the SAME rope all store an adopted StringImpl ref into
// the same word: ref leak at best, over-release/UAF of the published impl at
// worst, plus torn pairing of a stale isRope flag with the winner's pointer.
//
// Oracle: every thread forces resolution of the same shared ropes
// simultaneously and must observe the EXACT expected concatenation
// (charCodeAt probes + full equality + length). Any mismatch is a torn
// publication; the UAF arm shows up as a crash/ASAN fault. Deterministic
// content check; the race itself is amplified by the simultaneity gate and
// Tools/threads/amplify.sh.
//
// WRITTEN DURING BRING-UP: do not execute until the GIL-off ladder is up.
// This test is the acceptance check for landing handout §N.2.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 60;
const PIECES = 24;

function makePieces(round) {
    const pieces = [];
    for (let i = 0; i < PIECES; ++i) {
        // Vary widths so 8-bit and 16-bit lanes, substrings, and
        // multi-fiber ropes all get exercised.
        let p = "r" + round + "p" + i + "-";
        if (i % 5 === 0)
            p += "éሴ"; // force 16-bit
        pieces.push(p + "x".repeat(1 + ((round + i) % 40)));
    }
    return pieces;
}

const box = { rope: null, sub: null, expected: null, expectedSub: null,
              round: -1, stop: 0, started: 0 };
const gate = { go: 0 };

const threads = spawnN(THREADS, (id) => {
    Atomics.add(box, "started", 1);
    let rounds = 0;
    let last = -1;
    while (Atomics.load(box, "stop") === 0) {
        const r = Atomics.load(box, "round");
        if (r === last) {
            // Bounded yield; all threads wake on the round publication and
            // hit resolution of the same fresh rope near-simultaneously.
            Atomics.wait(gate, "go", 0, 2);
            continue;
        }
        last = r;
        const rope = box.rope;
        const sub = box.sub;
        const expected = box.expected;
        const expectedSub = box.expectedSub;
        if (rope === null)
            continue;

        // Force resolution through several distinct entry points:
        // charCodeAt (resolveRope), comparison (resolve + memcmp),
        // property lookup (resolveRopeToAtomString via toIdentifier).
        const len = rope.length;
        if (len !== expected.length)
            throw new Error("MC-TEAR S5: torn length: " + len + " vs "
                + expected.length + " (round " + r + ", thread " + id + ")");
        const probes = [0, 1, (len >> 1), len - 2, len - 1];
        for (const i of probes) {
            const c = rope.charCodeAt(i);
            if (c !== expected.charCodeAt(i))
                throw new Error("MC-TEAR S5: torn resolution at " + i + ": "
                    + c + " vs " + expected.charCodeAt(i) + " (round " + r
                    + ", thread " + id + ")");
        }
        if (rope !== expected) // full content compare; !== on equal content = tear
            throw new Error("MC-TEAR S5: resolved rope !== expected (round "
                + r + ", thread " + id + ")");
        if (sub !== expectedSub)
            throw new Error("MC-TEAR S5: resolved substring rope mismatch "
                + "(round " + r + ", thread " + id + ")");
        // Atomization lane: use the resolved string as a property key on a
        // private object (resolveRopeToAtomString against the sharded table).
        const o = {};
        o[sub] = id;
        if (o[expectedSub] !== id)
            throw new Error("MC-TEAR S5: atomized key mismatch (round " + r
                + ", thread " + id + ")");
        rounds++;
    }
    return rounds;
});

waitUntil(() => Atomics.load(box, "started") === THREADS);

for (let r = 0; r < ROUNDS; ++r) {
    const pieces = makePieces(r);
    // Build the rope WITHOUT resolving it on main: pure concatenation.
    let rope = pieces[0];
    for (let i = 1; i < PIECES; ++i)
        rope = rope + pieces[i];
    // Substring rope over an unresolved base exercises the substring fiber
    // path (fiber1/fiber2 never cleared post-publication).
    const lo = 3, hi = rope.length - 3;
    const sub = rope.substring(lo, hi);
    // Expected values, built piecewise via join so main does not resolve
    // the SAME rope cell the threads race on.
    const expected = pieces.join("");
    box.expected = expected;
    box.expectedSub = expected.substring(lo, hi);
    box.rope = rope;
    box.sub = sub;
    Atomics.store(box, "round", r);
    Atomics.notify(gate, "go");
    sleepMs(1);
}

Atomics.store(box, "stop", 1);
Atomics.notify(gate, "go");
const done = joinAll(threads);
for (const c of done)
    shouldBeTrue(c > 0, "every thread must have resolved at least one round");
print("mc-tear-rope-resolve-race: PASS (" + done.join(",") + " rounds)");
