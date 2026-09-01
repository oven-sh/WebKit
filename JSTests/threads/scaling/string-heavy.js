//@ requireOptions("--useJSThreads=1")
// string-heavy: rope building + flattening + atomization.
//
// Each thread repeatedly builds a multi-kilobyte rope from a small pool of
// leaf strings (+= concatenation => JSRopeString trees), forces resolution
// (charCodeAt / indexOf walk the rope, triggering flattening), then slices
// substrings out and uses them as plain-object property keys — the
// to-property-key path atomizes them. NO JS-level data is shared between
// threads, but identical substrings produced by different threads
// necessarily MEET in the engine's shared AtomStringTable: that incidental,
// engine-internal sharing is exactly the kind of hidden serialization the
// "program in parallel with itself" criterion exists to expose. The string
// content itself is deterministic, so the rolling hash checksum is identical
// across threads and runs.
//
// Gate class: standard thresholds (2.8x@4 / 4.5x@8). Rope leaves and
// flattened buffers are ordinary allocations; the dedicated GC-floor
// exemption belongs to splay-like alone.
//
// Work size targets roughly 1s per thread on a release build at scale 1 (the gate's sizing; standalone corpus runs default to a fractional CORPUS_DEFAULT_SCALE - see harness.js);
// fixed loop counts, no blocking ops.
load("./harness.js", "caller relative");

function stringWorkload() {
    const OUTER = Math.round(40000 * scalingWorkScale());
    const CONCATS_PER_ROPE = 700;
    const KEYS_PER_ROPE = 48;

    // Leaf pool: 64 distinct short strings, built deterministically. Mixed
    // lengths so rope fibers are irregular.
    const pieces = [];
    for (let i = 0; i < 64; ++i) {
        let piece = "";
        const len = 3 + (i % 7);
        for (let j = 0; j < len; ++j)
            piece += String.fromCharCode(97 + ((i * 31 + j * 7) % 26));
        pieces.push(piece);
    }

    let hash = 0x811c9dc5 >>> 0; // FNV-ish rolling hash, kept in uint32
    function mix(value) {
        hash = ((hash ^ (value & 0xffff)) * 0x01000193) >>> 0;
    }

    for (let o = 0; o < OUTER; ++o) {
        // 1) Rope building: a tree of CONCATS_PER_ROPE concatenations.
        let rope = "";
        for (let i = 0; i < CONCATS_PER_ROPE; ++i)
            rope += pieces[(i * 7 + o * 13) & 63];

        // 2) Flattening: rope-walking operations force resolution.
        mix(rope.charCodeAt((o * 97) % rope.length));
        mix(rope.indexOf(pieces[(o * 5) & 63], (o * 11) % 512));
        mix(rope.length);

        // 3) Atomization: substrings become property keys on a fresh table.
        //    Reading them back exercises the identifier lookup path too.
        //    Many of these keys repeat across iterations AND across threads
        //    (small leaf alphabet), so the shared atom table sees both
        //    fresh-insert and hit traffic from N threads at once.
        const table = {};
        const span = rope.length - 9;
        for (let k = 0; k < KEYS_PER_ROPE; ++k) {
            const key = rope.substring((k * 53 + o * 17) % span, (k * 53 + o * 17) % span + 8);
            table[key] = (table[key] === undefined ? 0 : table[key]) + 1;
        }
        let distinct = 0;
        let total = 0;
        for (const key in table) {
            distinct++;
            total += table[key];
            mix(key.charCodeAt(0) + key.charCodeAt(7));
        }
        shouldBe(total, KEYS_PER_ROPE, "string-heavy: key counts conserved");
        mix(distinct);
    }

    return "h" + hash;
}

runScalingWorkload("string-heavy", stringWorkload);

// WOULD-FAIL-IF: string infrastructure serializes or breaks under N
// mutators — most pointedly the shared AtomStringTable: a global lock held
// across atomization (rather than fine-grained/concurrent access) turns the
// property-key step into a serial section and collapses speedup below
// 2.8@4 / 4.5@8 in scaling-gate.sh --gate; rope flattening taking a shared
// lock or write-once fiber publication racing across threads does the same.
// NOTE the speedup half of this claim is live ONLY when the pinned --gate
// rung runs (see Tools/threads/INTEGRATE-scaling.md; default corpus runs
// check only the checksum half, plus the opt-in SCALING_SELF_TRIPWIRE in
// harness.js for gross re-serialization).
// Standalone, an atom-table race (two threads installing the same atom and
// getting DIFFERENT identities, or a torn flatten exposing partial buffers)
// breaks key identity in the table loop — the conservation shouldBe or the
// rolling-hash checksum against the single-thread reference trips on it.
