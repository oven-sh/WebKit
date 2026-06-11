//@ requireOptions("--useJSThreads=1")
// SPEC-vmstate W3 Group 4: regexp execution state (executingRegExp + the
// lazily-allocated regexp/BumpPointerAllocator stack) under thread churn.
// Each thread runs both shared and thread-local global regexps; lastIndex
// progression and capture contents must be exactly per-spec on every thread
// — corrupted per-thread regexp scratch state shows up as wrong captures,
// wrong lastIndex, or crashes in the Yarr interpreter/JIT.
load("../resources/assert.js", "caller relative");

const THREADS = 4;
const ROUNDS = 100;

// A shared (cross-thread) regexp object: its lastIndex is shared mutable
// state. Threads use it ONLY with fresh per-call lastIndex resets so results
// stay deterministic under the GIL's serialization.
const sharedRe = /(\d+)-(\w+)/;

const threads = spawnN(THREADS, t => {
    let digest = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        // Thread-local global regexp: full exec loop with lastIndex
        // progression, including a backreference and an alternation.
        const re = new RegExp("(x{1,3})y\\1|q(" + "z" + ")", "g");
        const s = "xxyxx_xyx_xxxyxxx_qz".repeat(3);
        let m;
        let count = 0;
        let capLen = 0;
        while ((m = re.exec(s))) {
            ++count;
            capLen += m[1] ? m[1].length : m[2].length;
        }
        // Per unit "xxyxx_xyx_xxxyxxx_qz": "xxyxx"(2), "xyx"(1),
        // "xxxyxxx"(3), "qz"(1) => 4 matches, capture lengths 2+1+3+1 = 7.
        if (count !== 12 || capLen !== 21)
            throw new Error("thread " + t + " regexp loop broke: " + count + "/" + capLen);
        digest += count + capLen;

        // String.prototype.replace with a function — exercises the regexp
        // stack reentrantly (the replacer itself runs a regexp).
        const out = "a1-bee a22-cee".replace(/(\d+)-(\w+)/g, (all, d, w) =>
            w.replace(/e/g, "E") + ":" + d.length);
        if (out !== "abEE:1 acEE:2")
            throw new Error("thread " + t + " nested replace broke: " + out);

        // Shared regexp, non-global: stateless match.
        const sm = sharedRe.exec("id " + (t * 1000 + r) + "-tag" + t);
        if (!sm || sm[1] !== String(t * 1000 + r) || sm[2] !== "tag" + t)
            throw new Error("thread " + t + " shared regexp broke");
        digest += sm[1].length;
    }
    return digest;
});

const results = joinAll(threads);
for (let t = 0; t < THREADS; ++t) {
    let expected = 0;
    for (let r = 0; r < ROUNDS; ++r)
        expected += 12 + 21 + String(t * 1000 + r).length;
    shouldBe(results[t], expected, "thread " + t + " digest");
}
