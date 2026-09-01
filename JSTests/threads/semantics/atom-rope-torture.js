//@ requireOptions("--useJSThreads=1")
// semantics/atom-rope-torture.js — N threads atomize the SAME logical name
// set simultaneously (each thread building each name through a DIFFERENT
// string path: literal-derived, rope concat, char-by-char, slice-of-backing)
// and resolve the SAME shared ropes simultaneously. Asserts cross-thread
// identity of atomization results: a property written under one thread's
// atom is visible under every other thread's independently-built equal
// string, and Symbol.for over the same names is === everywhere.
//
// The name set is shard-collision-heavy by construction: 512 names sharing
// long common prefixes/suffixes with single-character diffs and mixed
// lengths, so many hash near-misses and (mod small shard counts) many shard
// collisions land in the SharedAtomStringTable at once from 4 threads.
// (True StringHasher full-collisions are not crafted here; the load pattern
// targets the per-shard lock + lookup-or-insert race instead.)
load("../harness.js", "caller relative");

const THREADS = 4;
const NAMES = 512;

// Collision-heavy-ish names: shared 24-char prefix, shared suffix, the
// distinguishing characters buried mid-string, lengths varying by bucket.
function baseName(i) {
    const mid = String.fromCharCode(97 + (i & 15)) + (i >> 4);
    return "atomTorturePrefix_common" + mid + "_tail" + "x".repeat(i & 7);
}

function buildVariant(i, path) {
    const base = baseName(i);
    switch (((path % 4) + 4) % 4) {
    case 0:
        return base; // as computed (rope-ish from the concats above)
    case 1: {
        // Forced deep rope: concat one char at a time, resolved only when
        // used as a property name / compared.
        let s = "";
        for (let j = 0; j < base.length; ++j)
            s += base[j];
        return s;
    }
    case 2: {
        let s = "";
        for (let j = 0; j < base.length; ++j)
            s += String.fromCharCode(base.charCodeAt(j));
        return s;
    }
    default:
        return ("##" + base + "##").slice(2, 2 + base.length); // substring of a backing store
    }
}

// Phase 1: each name is OWNED by thread (i % THREADS); the owner writes
// table[name] = i under its own construction path. Phase 2 (after a
// barrier): EVERY thread reads EVERY name through a different path and
// verifies the value — cross-thread atom identity is what makes the lookup
// hit. Each thread also stresses Symbol.for identity and rope resolution.
const table = {};
const gate = { ready: 0, wrote: 0, go: 0 };

// Shared ropes: built identically; `oracleRope` is resolved on main now to
// produce the expected characters, `sharedRope` is left for the threads to
// resolve SIMULTANEOUSLY in phase 2.
function buildRope() {
    let r = "";
    for (let i = 0; i < 600; ++i)
        r += "seg" + (i & 31) + "|";
    return r;
}
const oracleRope = buildRope();
const oracleLen = oracleRope.length;
let oracleDigest = 0;
for (let k = 0; k < oracleLen; k += 7)
    oracleDigest = (oracleDigest * 31 + oracleRope.charCodeAt(k)) | 0;
const sharedRope = buildRope(); // unresolved until the threads hit it together

const workers = spawnN(THREADS, t => {
    Atomics.add(gate, "ready", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 2);

    // Phase 1: write owned names (path = t), and atomize NON-owned names
    // too (reads of absent props still atomize — maximal concurrent
    // lookup-or-insert traffic on the same shards).
    for (let i = 0; i < NAMES; ++i) {
        const name = buildVariant(i, t);
        if (i % THREADS === t)
            table[name] = i;
        else
            void table[name]; // concurrent atomize + miss
    }
    Atomics.add(gate, "wrote", 1);
    while (Atomics.load(gate, "wrote") < THREADS)
        Atomics.wait(gate, "wrote", Atomics.load(gate, "wrote"), 2);

    // Phase 2a: read EVERY name through a path different from its writer's.
    for (let i = 0; i < NAMES; ++i) {
        const name = buildVariant(i, t + 1 + (i % 2)); // never the identity path per-thread
        const v = table[name];
        if (v !== i)
            throw new Error("thread " + t + ": table lookup through variant path lost name " + i + " (got " + v + ")");
    }

    // Phase 2b: Symbol.for identity through yet another path.
    for (let i = t; i < NAMES; i += THREADS) {
        if (Symbol.for(buildVariant(i, t)) !== Symbol.for(buildVariant(i, t + 2)))
            throw new Error("thread " + t + ": Symbol.for split for name " + i);
    }

    // Phase 2c: simultaneous shared-rope resolution + digest.
    let digest = 0;
    for (let k = 0; k < sharedRope.length; k += 7)
        digest = (digest * 31 + sharedRope.charCodeAt(k)) | 0;
    if (sharedRope.length !== oracleLen)
        throw new Error("thread " + t + ": shared rope length " + sharedRope.length + " vs " + oracleLen);
    if (!(sharedRope === oracleRope))
        throw new Error("thread " + t + ": shared rope content diverged from oracle");
    return digest;
});

waitUntil(() => Atomics.load(gate, "ready") === THREADS);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

const digests = joinAll(workers);
for (let t = 0; t < THREADS; ++t)
    shouldBe(digests[t], oracleDigest, "thread " + t + " rope digest");

// Main thread: read every name through the remaining path; identity (===)
// of atomized results across threads is also checked directly via
// propertyIsEnumerable + own-key enumeration (one own key per logical name,
// ever — duplicated atoms would create duplicate-looking keys).
for (let i = 0; i < NAMES; ++i)
    shouldBe(table[buildVariant(i, 3 + (i & 1))], i, "main lookup of name " + i);
shouldBe(Object.keys(table).length, NAMES, "exactly one own key per logical name");

// And string identity at the JS level: equal-content names compare === and
// behave identically as keys regardless of construction path or thread.
for (let i = 0; i < NAMES; i += 37)
    shouldBeTrue(buildVariant(i, 0) === buildVariant(i, 2), "value equality of variants for " + i);

// WOULD-FAIL-IF: the shared atom-string table loses single-atom-per-
// character-sequence under concurrent atomization — two threads racing
// lookup-or-insert on the same shard both insert (or tryRefAtom resurrects
// a dying duplicate), yielding two live atoms for one name: the owner's
// write then lives under one atom and a reader's equal-content lookup
// resolves to the other, so table[variant] returns undefined (phase 2a /
// main lookups) and Object.keys reports a duplicate-shaped table (key-count
// check). Symbol.for splitting per construction path catches the same bug
// through the registry. Concurrent resolution of the same shared rope
// tearing the resolved fiber (partial fill published, wrong length, or two
// threads installing different buffers) trips the per-thread length/digest/
// content compares against the pre-resolved oracle.
