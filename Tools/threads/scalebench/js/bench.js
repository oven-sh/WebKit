// SCALEBENCH — JS implementation (jsc shell, Thread/Lock/Condition/Atomics-on-objects API).
// Implements Tools/threads/scalebench/SPEC.md exactly. Run:
//   WebKitBuild/Release/bin/jsc --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 \
//     --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 \
//     bench.js -- W
//
// Implementation decisions the spec leaves to the implementer (documented per §1.7):
//  - Shard maps are shared `Map`s: shared Map across Threads works on this branch
//    (verified: cross-thread Map.set/get under a Lock loses no updates).
//  - Posting lists are two parallel arrays { docIds: [], tfs: [] }. Go/Java must
//    use the analogous shape (slice/ArrayList pair or 2-field struct arrays).
//  - "firstLetter" for the Phase C group key (§1.10) is the first letter of the
//    base26 part, i.e. term.charAt(1): every term starts with the literal "t"
//    prefix, so term.charAt(0) would collapse the 26-letter cross to one letter
//    and contradict the spec's "up to 104 groups". Go/Java MUST match this.
//  - The §1.10 topN join "term:totalTf," is rendered as term + ":" + totalTf + ","
//    per entry, concatenated (trailing comma included). Go/Java MUST match this.
//  - Thread 0 is the main shell thread; it works as worker 0 and spawns W-1
//    Threads. total_ms = wall time from the start barrier to the completion of
//    checksumC (i.e. it includes the single-threaded inter-phase checksum/dfSnap
//    sections, which are part of the program in all three languages).
//  - Readers also bump the queriesDone counter (§1.7 lists it); it is not in the
//    output JSON but the atomic traffic must match across languages.

"use strict";

// serial-math-global-getbyidflush-loop: under the pinned GIL-off env,
// op_get_from_scope for the GlobalProperty `Math` does NOT fold —
// GetByStatus::computeFor(globalObject, structure, "Math") returns non-Simple
// flag-on, so DFGByteCodeParser.cpp:9881-9886 emits GetByIdFlush (R:World,
// W:Heap, ClobbersExit) for every `Math.imul` reference. LICM cannot hoist it
// across the loop-header CheckTraps (itself widened to write
// Watchpoint_fire/SideState/NamedProperties under useJSThreads&&!useThreadGIL,
// DFGClobberize.h:812+), so every iteration of postingsChecksumFlat / mix32 /
// fnv1a32 / next32 pays a full C++ generic-get-by-id for `Math`. Direct micro
// (pinned env, FTL-warm, single thread): `Math.imul` in-loop = 24.20 ns/it;
// hoisted-const imul = 0.54 ns/it (45×); pc-shaped loop 89.63 vs 2.04 ns/elem
// (44×); flag-off same loop 1.77 ns/elem. pc1+pc2 walks 8.72M postings × ~5
// `Math.imul` refs each — this hoist is the entire W=1 serial floor.
// Module-level `const` resolves as GlobalLexicalVar (DFGByteCodeParser.cpp
// :9895+): folds to a weakJSConstant via the SymbolTable watchpoint, or at
// worst a GetGlobalLexicalVariable direct-slot load — neither is GetByIdFlush.
// Pure arithmetic identity: $imul ≡ Math.imul, so the flat-arm checksum
// reference 686d6890|4154468|0fbbd673|3af6b072|e1d22021 is unchanged. Go/Java
// have no analogous global-property indirection for their imul, so this is a
// fairness correction, not a JS-only cheat.
const $imul = Math.imul;

// SCALEBENCH §44 Map-bimodal root cause was a `String.fromCharCode(97)` prewarm
// here (main-thread lazy-reification to dodge a foreign-TID Segmented
// StringConstructor butterfly → DFG GetButterfly BadIndexingType recompile
// loop). Removed per §45: the engine-side fix (ConcurrentButterfly §4.2
// noseg-property-only StayFlatShared + DFGByteCodeParser handleGetById
// BadIndexingType backstop) closes the hazard class, so the bench no longer
// needs to know which thread reifies first. See docs/threads/SCALEBENCH.md
// §44/§45 and MAP-BIMODAL-EVIDENCE.md for the full causal chain.

// ---------------------------------------------------------------------------
// §1.1 Constants
// ---------------------------------------------------------------------------
const MASK = 0xFFFFFFFFFFFFFFFFn;
const GLOBAL_SEED = 0x5CA1AB1E0BADF00Dn;
const QUERY_SEED = 0xFACEFEEDC0FFEE11n;
const GOLDEN = 0x9E3779B97F4A7C15n;
const V = 65536;
const Vn = 65536n;
const K = 128;
const Kn = 128n;
const WRITER_MOD = 10;
const TOPN = 20;

// Pinned by §8 calibration (2026-06-10, Release jsc, W=1): N_BASE=200000 gave
// Phase A = 105.4 s; 200000*15/105.41 -> 28000; re-measured Phase A = 14.70 s
// and 14.74 s (in [10, 20] s), identical checksums across runs.
const N_BASE_DEFAULT = 28000;
const N_BASE_SMOKE = 2000;    // SCALEBENCH_SMOKE=1 override (§5.1)

// W from the shell: jsc ... bench.js -- W
// (capture the shell-global `arguments` — inside functions, `arguments` would
// shadow it with the function's own arguments object)
const shellArgs = (typeof arguments !== "undefined") ? arguments : [];
const W = shellArgs.length > 0 ? parseInt(shellArgs[0], 10) : 1;
if (!(W >= 1))
    throw new Error("usage: jsc <flags> bench.js -- W");

// SCALEBENCH_SMOKE (§5.1): the jsc shell has NO env accessor, and its readFile
// is st_size-based so /proc/self/environ reads back empty (procfs reports
// size 0). We still try environ (in case the shell ever grows a streaming
// read), but run.sh MUST additionally pass the literal extra argument "smoke"
// for the JS smoke leg:  jsc <flags> bench.js -- W smoke
function isSmoke() {
    try {
        const env = readFile("/proc/self/environ");
        if (env.split("\0").indexOf("SCALEBENCH_SMOKE=1") >= 0)
            return true;
    } catch (e) {
        // fall through
    }
    return shellArgs.length > 1 && shellArgs[1] === "smoke";
}
const N_BASE = isSmoke() ? N_BASE_SMOKE : N_BASE_DEFAULT;

// SCALEBENCH intcs arm (§39b full-workload 32-bit, supersedes the §29/§39
// checksum-only reroute): `bench.js -- W intcs` runs the SAME work shape as
// the spec arm — string concat → tokenize → Map<string>.get/set under shard
// locks → 90/10 query mix → Phase C group-by — but EVERY numeric operation is
// 32-bit Number arithmetic (Math.imul / >>>0): splitmix32 PRNG (next32),
// docSeedI/qSeedI = mix32(seed^d), pickTermI, shardOfI = fnv1a32%K, mix32
// everywhere, fnv1a32 for all string hashing, 32-bit per-thread partialB.
// ZERO BigInt on the intcs path. This produces DIFFERENT documents than the
// spec u64 arm (different PRNG output) and so a NEW cross-language reference
// tuple — recorded in docs/threads/SCALEBENCH.md §39b. Go (uint32) and Java
// (int + >>>) implement byte-identical arithmetic. Default OFF: no arg ⇒ the
// spec-exact BigInt path runs and the b3e65a68… reference tuple is unchanged.
const INTCS = shellArgs.indexOf("intcs") >= 1;

// SCALEBENCH §40 bisect arms — both compose with the intcs 32-bit base
// (next32/mix32/fnv1a32/docSeedI/qSeedI/pickTermI/shardOfI), both opt-in,
// both produce a NEW cross-language checksum reference. Either flag forces
// the intcs numeric base; neither composes with FLAT.
//
//   noconcat: is genDoc-concat → tokenize the cost? genDocTermsI returns the
//     term STRINGS directly (skip building doc text + tokenizing it back).
//     Everything downstream of the token list — tf Map<string>, shardOfI,
//     shard.map.get/set, queries, phaseC — is byte-identical to intcs.
//
//   nomap: is Map<string>.get/set the cost? Full string round-trip KEPT
//     (genDocTextI → tokenize), but every Map<string> lookup is replaced
//     with a direct integer index: invTermOf(token) base26-decodes back to
//     termId; per-doc tf is an Int32Array(V)+dirty-list (flat-arm shape);
//     per-term storage is a global V-slot array nmPost[termId] (NOT a
//     Map<string>); shardOf is still fnv1a32(term)%K via a precomputed
//     nmShardOf[V] table so the lock-contention pattern matches intcs
//     exactly. Queries/phaseC walk by termId; termOf(id) is rebuilt only
//     for the phaseC topN join string.
const NOCONCAT = shellArgs.indexOf("noconcat") >= 1;
const NOMAP = shellArgs.indexOf("nomap") >= 1;

// SCALEBENCH flat arm (§34 discriminant — "if TC39 stage-2 structs were
// implemented and used effectively"): `bench.js -- W flat` runs the SAME
// concurrency surface (W Threads, K shard Locks, the barrier, the Atomics
// counters, the 90/10 query mix, the 104-group Phase C lock fan-in) but with
// EVERY boxed-object / Map<string> / heap-BigInt cost removed from the hot
// path — Number-only splitmix32 PRNG, termId ints emitted directly (no
// termOf/genDocText/tokenize strings), shardOf = termId&127, per-shard flat
// linked-list-in-Int32Arrays, Int32Array tf/df/cand scratch, 32-bit mix
// checksums. Produces its OWN checksum reference (32-bit, not the spec
// 64-bit). Default OFF: no arg => the spec-exact path runs and the §1.1
// reference tuple is unchanged. The point of this arm: if "flat" scales like
// Go, the gap is the object model; if it scales like JS-intcs, the gap is
// our concurrency/GC/scheduling.
const FLAT = shellArgs.indexOf("flat") >= 1;
const N_QUERIES = N_BASE;
const N_BASEn = BigInt(N_BASE);

// ---------------------------------------------------------------------------
// §1.2 PRNG — splitmix64 (exact); §1.3 FNV-1a 64
// ---------------------------------------------------------------------------
function next(p) {
    p.s = (p.s + GOLDEN) & MASK;
    let z = p.s;
    z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & MASK;
    z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & MASK;
    return z ^ (z >> 31n);
}

function mix(x) {
    let z = (x + GOLDEN) & MASK;
    z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & MASK;
    z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & MASK;
    return z ^ (z >> 31n);
}

function randBelow(r, nBig) {
    return Number(r % nBig);
}

const FNV_OFFSET = 0xCBF29CE484222325n;
const FNV_PRIME = 0x100000001B3n;
const BYTE = [];
for (let i = 0; i < 256; ++i)
    BYTE.push(BigInt(i));

function fnv1a(s) {
    let h = FNV_OFFSET;
    for (let i = 0; i < s.length; ++i)
        h = ((h ^ BYTE[s.charCodeAt(i)]) * FNV_PRIME) & MASK;
    return h;
}

// Feed a u64 as 8 little-endian bytes into a running FNV-1a hash.
function fnvU64LE(h, x) {
    for (let i = 0; i < 8; ++i) {
        h = ((h ^ (x & 0xFFn)) * FNV_PRIME) & MASK;
        x >>= 8n;
    }
    return h;
}

// PRNG test vector (§7): seed 0 -> e220a8397b1dcdaf, 6e789e6aa1b965f4, 06c45d188009454f.
{
    const p = { s: 0n };
    const want = [0xE220A8397B1DCDAFn, 0x6E789E6AA1B965F4n, 0x06C45D188009454Fn];
    for (let i = 0; i < 3; ++i) {
        if (next(p) !== want[i])
            throw new Error("splitmix64 self-test failed at output " + i);
    }
    if (fnv1a("a") !== 0xAF63DC4C8601EC8Cn)
        throw new Error("fnv1a self-test failed");
}

// ---------------------------------------------------------------------------
// §1.4 Vocabulary
// ---------------------------------------------------------------------------
const A_CODE = "a".charCodeAt(0);
function termOf(i) {
    let s = "";
    do {
        s = String.fromCharCode(A_CODE + (i % 26)) + s;
        i = (i / 26) | 0;
    } while (i > 0);
    return "t" + s;
}

function pickTerm(p) {
    const a = randBelow(next(p), Vn);
    const b = randBelow(next(p), Vn);
    return a < b ? a : b;
}

// ---------------------------------------------------------------------------
// §1.5 Document grammar; §1.6 tokenizer
// ---------------------------------------------------------------------------
function docSeed(d) {
    return mix(GLOBAL_SEED ^ ((BigInt(d) * GOLDEN) & MASK));
}

// Doc-text assembly at the SPEC §1.5 pinned abstraction level (hand-rolled,
// builder-style — matches Go strings.Builder / Java StringBuilder): pieces are
// appended to a parts array and joined ONCE at the end; capitalization is
// charCode arithmetic (code - 32, ASCII a-z only, like Go `t[0] - 32` and Java
// `(char)(c - 32)`); NO intermediate per-token concatenations (no
// `tok + ","`-style temporaries — Go/Java never allocate those).
function genDocText(d) {
    const p = { s: docSeed(d) };
    const titleLen = 5 + randBelow(next(p), 8n);
    const bodyLen = 80 + randBelow(next(p), 121n);
    const n = titleLen + bodyLen;
    const parts = [];
    for (let j = 0; j < n; ++j) {
        const tok = termOf(pickTerm(p));
        if (j > 0)
            parts.push(" ");
        if (j % 7 === 0) {
            parts.push(String.fromCharCode(tok.charCodeAt(0) - 32)); // capitalize (ASCII)
            parts.push(tok.substring(1));
        } else {
            parts.push(tok);
        }
        if (j % 11 === 3)
            parts.push(",");
        if (j % 13 === 12)
            parts.push(".");
    }
    return parts.join("");
}

// Tokenizer at the SPEC §1.6 pinned abstraction level (hand-rolled, matches
// go/main.go tokenize() and Bench.java tokenize() exactly): explicit charCodeAt
// scan loop (NO regex split, NO Unicode-aware String.prototype.toLowerCase),
// manual ASCII +32 lowercasing, and exactly two allocations per token — the
// raw split piece (substring) and an unconditionally allocated lowercased
// copy (fromCharCode over a scratch code array; never skipped when already
// lowercase).
function tokenize(text) {
    const out = [];
    const n = text.length;
    let start = -1;
    for (let i = 0; i <= n; ++i) {
        const c = i < n ? text.charCodeAt(i) : 32;
        const alnum = (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || (c >= 48 && c <= 57);
        if (alnum) {
            if (start < 0)
                start = i;
            continue;
        }
        if (start < 0)
            continue;
        const raw = text.substring(start, i); // split allocation
        start = -1;
        const codes = []; // scratch, analogous to Go's lc []byte / Java's char[]
        for (let k = 0; k < raw.length; ++k) {
            let ch = raw.charCodeAt(k);
            if (ch >= 65 && ch <= 90)
                ch += 32;
            codes.push(ch);
        }
        out.push(String.fromCharCode(...codes)); // lowercase allocation (always)
    }
    return out;
}

// ---------------------------------------------------------------------------
// §1.7 Shared state
// ---------------------------------------------------------------------------
const shards = [];
for (let i = 0; i < K; ++i)
    shards.push({ lock: new Lock(), map: new Map() });

function shardOf(term) {
    return Number(fnv1a(term) % Kn);
}

const counters = {
    nextDoc: 0,
    nextOp: 0,
    nextShard: 0,
    docsIngested: 0,
    tokensProcessed: 0,
    queriesDone: 0,
    writesDone: 0,
};

// Published by thread 0 at the A/B barrier; read-only thereafter.
const published = { dfSnap: null, groups: null, groupKeys: null };

// Per-thread Phase B checksum partials (slot per worker; summed by thread 0).
const partialB = [];
for (let i = 0; i < W; ++i)
    partialB.push(0n);

// ---------------------------------------------------------------------------
// Barrier — hand-rolled counting barrier, Lock + Condition (§1)
// ---------------------------------------------------------------------------
const barrier = {
    n: W,
    count: 0,
    gen: 0,
    lock: new Lock(),
    cond: new Condition(),
    await() {
        this.lock.hold(() => {
            const g = this.gen;
            if (++this.count === this.n) {
                this.count = 0;
                this.gen++;
                this.cond.notifyAll();
            } else {
                while (this.gen === g)
                    this.cond.wait(this.lock);
            }
        });
    },
};

// ---------------------------------------------------------------------------
// §1.8 Phase A — INGEST
// ---------------------------------------------------------------------------
function ingestDoc(d) {
    const text = genDocText(d);
    const toks = tokenize(text);
    const tf = new Map();
    for (let i = 0; i < toks.length; ++i) {
        const t = toks[i];
        const cur = tf.get(t);
        tf.set(t, cur === undefined ? 1 : cur + 1);
    }
    for (const [term, count] of tf) {
        const shard = shards[shardOf(term)];
        shard.lock.hold(() => {
            let p = shard.map.get(term);
            if (p === undefined) {
                p = { docIds: [], tfs: [] };
                shard.map.set(term, p);
            }
            p.docIds.push(d);
            p.tfs.push(count);
        });
    }
    Atomics.add(counters, "tokensProcessed", toks.length);
    Atomics.add(counters, "docsIngested", 1);
}

function phaseA() {
    for (;;) {
        const d = Atomics.add(counters, "nextDoc", 1);
        if (d >= N_BASE)
            break;
        ingestDoc(d);
    }
}

// Postings checksum (§1.8) — order-independent mod-2^64 sum. Also counts postings.
function postingsChecksum() {
    let sum = 0n;
    let postings = 0;
    for (let s = 0; s < K; ++s) {
        for (const [term, p] of shards[s].map) {
            const th = fnv1a(term);
            const n = p.docIds.length;
            postings += n;
            for (let i = 0; i < n; ++i) {
                const item = th
                    ^ ((BigInt(p.docIds[i]) * 0xD6E8FEB86659FD93n) & MASK)
                    ^ ((BigInt(p.tfs[i]) * 0xCAAF00DDn) & MASK);
                sum = (sum + mix(item)) & MASK;
            }
        }
    }
    return { sum, postings };
}

// intcs arm: same loop shape, allocation-free 32-bit Number arithmetic.
// fnv1a32/mix32 mirror the BigInt versions structurally (xorshift-multiply)
// but mod-2^32 via >>>0 / Math.imul. No BigInt() boxing, no heap JSBigInt.
function fnv1a32(s) {
    let h = 0x811c9dc5 >>> 0;
    for (let i = 0; i < s.length; ++i)
        h = $imul(h ^ s.charCodeAt(i), 0x01000193) >>> 0;
    return h;
}
function mix32(x) {
    let z = (x + 0x9e3779b9) >>> 0;
    z = $imul(z ^ (z >>> 16), 0x85ebca6b) >>> 0;
    z = $imul(z ^ (z >>> 13), 0xc2b2ae35) >>> 0;
    return (z ^ (z >>> 16)) >>> 0;
}
function postingsChecksumInt() {
    let sum = 0;
    let postings = 0;
    for (let s = 0; s < K; ++s) {
        for (const [term, p] of shards[s].map) {
            const th = fnv1a32(term);
            const n = p.docIds.length;
            postings += n;
            for (let i = 0; i < n; ++i) {
                const item = (th
                    ^ ($imul(p.docIds[i], 0xd6e8feb9) >>> 0)
                    ^ ($imul(p.tfs[i], 0xcaaf00dd) >>> 0)) >>> 0;
                sum = (sum + mix32(item)) >>> 0;
            }
        }
    }
    return { sum: BigInt(sum), postings };
}

function buildDfSnap() {
    const snap = new Map();
    for (let s = 0; s < K; ++s) {
        for (const [term, p] of shards[s].map)
            snap.set(term, p.docIds.length);
    }
    return snap;
}

// ---------------------------------------------------------------------------
// §1.9 Phase B — QUERY (90% read / 10% write)
// ---------------------------------------------------------------------------
function copyPostings(term) {
    const shard = shards[shardOf(term)];
    let docIds = null, tfs = null;
    shard.lock.hold(() => {
        const p = shard.map.get(term);
        if (p !== undefined) {
            docIds = p.docIds.slice();
            tfs = p.tfs.slice();
        }
    });
    return docIds === null ? { docIds: [], tfs: [] } : { docIds, tfs };
}

function queryPoint(p) {
    const term = termOf(pickTerm(p));
    const shard = shards[shardOf(term)];
    let df = 0, sumTf = 0;
    shard.lock.hold(() => {
        const pl = shard.map.get(term);
        if (pl !== undefined) {
            for (let i = 0; i < pl.docIds.length; ++i) {
                if (pl.docIds[i] < N_BASE) {
                    df++;
                    sumTf += pl.tfs[i];
                }
            }
        }
    });
    return mix(fnv1a(term) ^ ((BigInt(df) * 0x9E37n) & MASK) ^ BigInt(sumTf));
}

function queryAnd(p) {
    const nTerms = 2 + randBelow(next(p), 2n);
    const terms = [];
    for (let i = 0; i < nTerms; ++i)
        terms.push(termOf(pickTerm(p)));
    const lists = [];
    for (let i = 0; i < nTerms; ++i)
        lists.push(copyPostings(terms[i])); // copy under shard lock (§1.9)
    const cand = new Map();
    const first = lists[0];
    for (let i = 0; i < first.docIds.length; ++i) {
        const d = first.docIds[i];
        if (d < N_BASE)
            cand.set(d, 1);
    }
    for (let k = 1; k < nTerms; ++k) {
        const list = lists[k];
        for (let i = 0; i < list.docIds.length; ++i) {
            const d = list.docIds[i];
            if (d < N_BASE && cand.get(d) === k)
                cand.set(d, k + 1);
        }
    }
    let matchSum = 0n;
    let matchCount = 0;
    for (const [d, c] of cand) {
        if (c === nTerms) {
            matchSum = (matchSum + mix(BigInt(d))) & MASK;
            matchCount++;
        }
    }
    return mix(matchSum) ^ BigInt(matchCount);
}

function queryScored(p, dfSnap) {
    const nTerms = 2 + randBelow(next(p), 2n);
    const terms = [];
    for (let i = 0; i < nTerms; ++i)
        terms.push(termOf(pickTerm(p)));
    // Union of the terms' base postings, scored with the frozen dfSnap.
    const cand = new Map();
    for (let k = 0; k < nTerms; ++k) {
        const term = terms[k];
        const df = dfSnap.get(term);
        const idf = df === undefined ? 0n : (N_BASEn * 1000n) / BigInt(df);
        const list = copyPostings(term);
        for (let i = 0; i < list.docIds.length; ++i) {
            const d = list.docIds[i];
            if (d < N_BASE) {
                const cur = cand.get(d);
                const add = (BigInt(list.tfs[i]) * idf) & MASK;
                cand.set(d, cur === undefined ? add : (cur + add) & MASK);
            }
        }
    }
    const entries = [];
    for (const [d, score] of cand)
        entries.push({ d, score });
    entries.sort((a, b) => {
        if (a.score !== b.score)
            return a.score > b.score ? -1 : 1; // score DESC (unsigned u64)
        return a.d - b.d;                      // docId ASC
    });
    const top = entries.length < TOPN ? entries : entries.slice(0, TOPN);
    let h = FNV_OFFSET;
    for (let i = 0; i < top.length; ++i)
        h = fnvU64LE(h, BigInt(top[i].d));
    for (let i = 0; i < top.length; ++i)
        h = fnvU64LE(h, top[i].score);
    return h;
}

function phaseB(id) {
    const dfSnap = published.dfSnap;
    let cs = 0n;
    for (;;) {
        const q = Atomics.add(counters, "nextOp", 1);
        if (q >= N_QUERIES)
            break;
        if (q % WRITER_MOD === 0) {
            ingestDoc(N_BASE + q / WRITER_MOD); // §1.9 writer: same grammar as Phase A
            Atomics.add(counters, "writesDone", 1);
            continue;
        }
        const p = { s: mix(QUERY_SEED ^ ((BigInt(q) * GOLDEN) & MASK)) };
        const kind = randBelow(next(p), 10n);
        let perQueryHash;
        if (kind <= 3)
            perQueryHash = queryPoint(p);
        else if (kind <= 7)
            perQueryHash = queryAnd(p);
        else
            perQueryHash = queryScored(p, dfSnap);
        cs = (cs + mix(((BigInt(q) * GOLDEN) & MASK) ^ perQueryHash)) & MASK;
        Atomics.add(counters, "queriesDone", 1);
    }
    partialB[id] = cs;
}

// ---------------------------------------------------------------------------
// §1.10 Phase C — ANALYTICS
// ---------------------------------------------------------------------------
function makeGroups() {
    const groups = new Map();
    const keys = [];
    for (let len = 2; len <= 5; ++len) {
        for (let c = 0; c < 26; ++c) {
            const key = len + ":" + String.fromCharCode(A_CODE + c);
            keys.push(key);
            // totalTf/df stay Numbers (§7: BigInt ONLY in PRNG/hash paths —
            // group totals are bounded by total tokens, far below 2^53);
            // converted to BigInt only inside checksumPhaseC.
            groups.set(key, { lock: new Lock(), totalTf: 0, df: 0, terms: [] });
        }
    }
    return { groups, keys };
}

function phaseC() {
    const groups = published.groups;
    for (;;) {
        const s = Atomics.add(counters, "nextShard", 1);
        if (s >= K)
            break;
        for (const [term, p] of shards[s].map) {
            let totalTf = 0;
            const df = p.docIds.length; // ALL postings: base + writer docs
            for (let i = 0; i < df; ++i)
                totalTf += p.tfs[i];
            // Group key: string length crossed with the first letter of the
            // base26 part (term.charAt(1) — see header note).
            const key = term.length + ":" + term.charAt(1);
            const g = groups.get(key);
            g.lock.hold(() => {
                g.totalTf += totalTf; // Number arithmetic under the lock (§7)
                g.df += df;
                g.terms.push({ term, totalTf });
            });
        }
    }
}

function checksumPhaseC() {
    const groups = published.groups;
    const keys = published.groupKeys;
    let sum = 0n;
    for (let i = 0; i < keys.length; ++i) {
        const key = keys[i];
        const g = groups.get(key);
        g.terms.sort((a, b) => {
            if (a.totalTf !== b.totalTf)
                return b.totalTf - a.totalTf; // totalTf DESC
            return a.term < b.term ? -1 : (a.term > b.term ? 1 : 0); // term ASC
        });
        const top = g.terms.length < TOPN ? g.terms : g.terms.slice(0, TOPN);
        let joined = "";
        for (let j = 0; j < top.length; ++j)
            joined += top[j].term + ":" + top[j].totalTf + ",";
        const item = fnv1a(key)
            ^ BigInt(g.totalTf)
            ^ ((BigInt(g.df) * GOLDEN) & MASK)
            ^ fnv1a(joined);
        sum = (sum + mix(item)) & MASK;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// intcs ARM — §39b full-workload 32-bit (see the INTCS comment near the top
// of the file). Reuses next32/mix32/fnv1a32 (defined above and in the flat
// arm; function declarations are hoisted), termOf, tokenize, shards, counters,
// buildDfSnap, makeGroups, phaseC (none of which touch BigInt). Everything
// below is gated on INTCS; when INTCS is false none of this is reachable and
// the spec-exact u64 path runs unchanged. Go (uint32) and Java (int + >>>)
// must implement byte-identical arithmetic for every function here.
// ---------------------------------------------------------------------------
const GSEED32 = 0x5ca1ab1e >>> 0;
const QSEED32 = 0xfacefeed >>> 0;
const GOLDEN32 = 0x9e3779b9 >>> 0;

function docSeedI(d) { return mix32((GSEED32 ^ d) >>> 0); }
function qSeedI(q)   { return mix32((QSEED32 ^ q) >>> 0); }
function shardOfI(term) { return fnv1a32(term) % K; }
function pickTermI(p) {
    const a = next32(p) & (V - 1);
    const b = next32(p) & (V - 1);
    return a < b ? a : b;
}
// Feed a u32 as 4 little-endian bytes into a running FNV-1a-32 hash.
function fnvU32LE(h, x) {
    for (let i = 0; i < 4; ++i) {
        h = $imul(h ^ (x & 0xff), 0x01000193) >>> 0;
        x = x >>> 8;
    }
    return h;
}

function genDocTextI(d) {
    const p = { s: docSeedI(d) };
    const titleLen = 5 + (next32(p) % 8);
    const bodyLen = 80 + (next32(p) % 121);
    const n = titleLen + bodyLen;
    const parts = [];
    for (let j = 0; j < n; ++j) {
        const tok = termOf(pickTermI(p));
        if (j > 0)
            parts.push(" ");
        if (j % 7 === 0) {
            parts.push(String.fromCharCode(tok.charCodeAt(0) - 32));
            parts.push(tok.substring(1));
        } else {
            parts.push(tok);
        }
        if (j % 11 === 3)
            parts.push(",");
        if (j % 13 === 12)
            parts.push(".");
    }
    return parts.join("");
}

function ingestDocI(d) {
    const text = genDocTextI(d);
    const toks = tokenize(text);
    const tf = new Map();
    for (let i = 0; i < toks.length; ++i) {
        const t = toks[i];
        const cur = tf.get(t);
        tf.set(t, cur === undefined ? 1 : cur + 1);
    }
    for (const [term, count] of tf) {
        const shard = shards[shardOfI(term)];
        shard.lock.hold(() => {
            let pl = shard.map.get(term);
            if (pl === undefined) {
                pl = { docIds: [], tfs: [] };
                shard.map.set(term, pl);
            }
            pl.docIds.push(d);
            pl.tfs.push(count);
        });
    }
    Atomics.add(counters, "tokensProcessed", toks.length);
    Atomics.add(counters, "docsIngested", 1);
}

function phaseAI() {
    for (;;) {
        const d = Atomics.add(counters, "nextDoc", 1);
        if (d >= N_BASE)
            break;
        ingestDocI(d);
    }
}

function copyPostingsI(term) {
    const shard = shards[shardOfI(term)];
    let docIds = null, tfs = null;
    shard.lock.hold(() => {
        const p = shard.map.get(term);
        if (p !== undefined) {
            docIds = p.docIds.slice();
            tfs = p.tfs.slice();
        }
    });
    return docIds === null ? { docIds: [], tfs: [] } : { docIds, tfs };
}

function queryPointI(p) {
    const term = termOf(pickTermI(p));
    const shard = shards[shardOfI(term)];
    let df = 0, sumTf = 0;
    shard.lock.hold(() => {
        const pl = shard.map.get(term);
        if (pl !== undefined) {
            for (let i = 0; i < pl.docIds.length; ++i) {
                if (pl.docIds[i] < N_BASE) {
                    df++;
                    sumTf += pl.tfs[i];
                }
            }
        }
    });
    return mix32((fnv1a32(term) ^ $imul(df, 0x9e37) ^ sumTf) >>> 0);
}

function queryAndI(p) {
    const nTerms = 2 + (next32(p) % 2);
    const terms = [];
    for (let i = 0; i < nTerms; ++i)
        terms.push(termOf(pickTermI(p)));
    const lists = [];
    for (let i = 0; i < nTerms; ++i)
        lists.push(copyPostingsI(terms[i]));
    const cand = new Map();
    const first = lists[0];
    for (let i = 0; i < first.docIds.length; ++i) {
        const d = first.docIds[i];
        if (d < N_BASE)
            cand.set(d, 1);
    }
    for (let k = 1; k < nTerms; ++k) {
        const list = lists[k];
        for (let i = 0; i < list.docIds.length; ++i) {
            const d = list.docIds[i];
            if (d < N_BASE && cand.get(d) === k)
                cand.set(d, k + 1);
        }
    }
    let matchSum = 0;
    let matchCount = 0;
    for (const [d, c] of cand) {
        if (c === nTerms) {
            matchSum = (matchSum + mix32(d)) >>> 0;
            matchCount++;
        }
    }
    return (mix32(matchSum) ^ matchCount) >>> 0;
}

function queryScoredI(p, dfSnap) {
    const nTerms = 2 + (next32(p) % 2);
    const terms = [];
    for (let i = 0; i < nTerms; ++i)
        terms.push(termOf(pickTermI(p)));
    const cand = new Map();
    for (let k = 0; k < nTerms; ++k) {
        const term = terms[k];
        const df = dfSnap.get(term);
        const idf = df === undefined ? 0 : ((N_BASE * 1000 / df) | 0);
        const list = copyPostingsI(term);
        for (let i = 0; i < list.docIds.length; ++i) {
            const d = list.docIds[i];
            if (d < N_BASE) {
                const cur = cand.get(d);
                const add = $imul(list.tfs[i], idf) >>> 0;
                cand.set(d, cur === undefined ? add : (cur + add) >>> 0);
            }
        }
    }
    const entries = [];
    for (const [d, score] of cand)
        entries.push({ d, score });
    entries.sort((a, b) => {
        if (a.score !== b.score)
            return a.score > b.score ? -1 : 1; // score DESC (unsigned u32)
        return a.d - b.d;                      // docId ASC
    });
    const top = entries.length < TOPN ? entries : entries.slice(0, TOPN);
    let h = 0x811c9dc5 >>> 0;
    for (let i = 0; i < top.length; ++i)
        h = fnvU32LE(h, top[i].d);
    for (let i = 0; i < top.length; ++i)
        h = fnvU32LE(h, top[i].score);
    return h;
}

const partialBI = [];
for (let i = 0; i < W; ++i)
    partialBI.push(0);

function phaseBI(id) {
    const dfSnap = published.dfSnap;
    let cs = 0;
    for (;;) {
        const q = Atomics.add(counters, "nextOp", 1);
        if (q >= N_QUERIES)
            break;
        if (q % WRITER_MOD === 0) {
            ingestDocI(N_BASE + q / WRITER_MOD);
            Atomics.add(counters, "writesDone", 1);
            continue;
        }
        const p = { s: qSeedI(q) };
        const kind = next32(p) % 10;
        let perQueryHash;
        if (kind <= 3)
            perQueryHash = queryPointI(p);
        else if (kind <= 7)
            perQueryHash = queryAndI(p);
        else
            perQueryHash = queryScoredI(p, dfSnap);
        cs = (cs + mix32(($imul(q, GOLDEN32) ^ perQueryHash) >>> 0)) >>> 0;
        Atomics.add(counters, "queriesDone", 1);
    }
    partialBI[id] = cs;
}

// ---------------------------------------------------------------------------
// §40 NOCONCAT ARM — intcs base with the genDocTextI→tokenize round-trip
// removed: genDocTermsI emits the term strings directly.
// ---------------------------------------------------------------------------
function genDocTermsI(d) {
    const p = { s: docSeedI(d) };
    const titleLen = 5 + (next32(p) % 8);
    const bodyLen = 80 + (next32(p) % 121);
    const n = titleLen + bodyLen;
    const toks = [];
    for (let j = 0; j < n; ++j)
        toks.push(termOf(pickTermI(p)));
    return toks;
}

function ingestDocNC(d) {
    const toks = genDocTermsI(d);
    const tf = new Map();
    for (let i = 0; i < toks.length; ++i) {
        const t = toks[i];
        const cur = tf.get(t);
        tf.set(t, cur === undefined ? 1 : cur + 1);
    }
    for (const [term, count] of tf) {
        const shard = shards[shardOfI(term)];
        shard.lock.hold(() => {
            let pl = shard.map.get(term);
            if (pl === undefined) {
                pl = { docIds: [], tfs: [] };
                shard.map.set(term, pl);
            }
            pl.docIds.push(d);
            pl.tfs.push(count);
        });
    }
    Atomics.add(counters, "tokensProcessed", toks.length);
    Atomics.add(counters, "docsIngested", 1);
}

function phaseANC() {
    for (;;) {
        const d = Atomics.add(counters, "nextDoc", 1);
        if (d >= N_BASE)
            break;
        ingestDocNC(d);
    }
}

function phaseBNC(id) {
    const dfSnap = published.dfSnap;
    let cs = 0;
    for (;;) {
        const q = Atomics.add(counters, "nextOp", 1);
        if (q >= N_QUERIES)
            break;
        if (q % WRITER_MOD === 0) {
            ingestDocNC(N_BASE + q / WRITER_MOD);
            Atomics.add(counters, "writesDone", 1);
            continue;
        }
        const p = { s: qSeedI(q) };
        const kind = next32(p) % 10;
        let perQueryHash;
        if (kind <= 3)
            perQueryHash = queryPointI(p);
        else if (kind <= 7)
            perQueryHash = queryAndI(p);
        else
            perQueryHash = queryScoredI(p, dfSnap);
        cs = (cs + mix32(($imul(q, GOLDEN32) ^ perQueryHash) >>> 0)) >>> 0;
        Atomics.add(counters, "queriesDone", 1);
    }
    partialBI[id] = cs;
}

// ---------------------------------------------------------------------------
// §40 NOMAP ARM — intcs base with every Map<string> lookup replaced by a
// direct integer index. Full string round-trip (genDocTextI → tokenize) KEPT.
// ---------------------------------------------------------------------------

// invTermOf: base26-decode the lowercased token back to its termId.
// termOf(i) = "t" + base26(i) MSD-first, so strip the leading 't' and fold.
function invTermOf(s) {
    let r = 0;
    for (let i = 1; i < s.length; ++i)
        r = r * 26 + (s.charCodeAt(i) - A_CODE);
    return r;
}

// firstLetterIdx: index (0..25) of term.charAt(1), i.e. the most-significant
// base26 digit of termId — computed without building the string.
function firstLetterIdx(t) {
    let x = t;
    while (x >= 26) x = (x / 26) | 0;
    return x;
}

// nmShardOf[t] = fnv1a32(termOf(t)) % K, precomputed once so queries never
// rebuild a term string just to find its shard lock. Same lock-contention
// pattern as intcs by construction.
const nmShardOf = NOMAP ? new Int32Array(V) : null;
const nmPost = NOMAP ? new Array(V) : null; // nmPost[t] = {docIds:[], tfs:[]} | undefined
if (NOMAP) {
    for (let t = 0; t < V; ++t)
        nmShardOf[t] = fnv1a32(termOf(t)) % K;
}

// Per-worker scratch (allocated ON the worker thread): tf accumulator +
// dirty-list, flat-arm shape.
const nmScratch = [];
for (let i = 0; i < W; ++i) nmScratch.push(null);
function makeNMScratch() {
    return { tf: new Int32Array(V), dirty: new Int32Array(256) };
}

function ingestDocNM(d, sc) {
    const text = genDocTextI(d);          // full string concat KEPT
    const toks = tokenize(text);          // full tokenize KEPT
    const tf = sc.tf, dirty = sc.dirty;
    let nDirty = 0;
    for (let i = 0; i < toks.length; ++i) {
        const t = invTermOf(toks[i]);
        if (tf[t] === 0) dirty[nDirty++] = t;
        tf[t]++;
    }
    for (let k = 0; k < nDirty; ++k) {
        const t = dirty[k];
        const count = tf[t];
        tf[t] = 0;
        const shard = shards[nmShardOf[t]];
        shard.lock.hold(() => {
            let pl = nmPost[t];
            if (pl === undefined) {
                pl = { docIds: [], tfs: [] };
                nmPost[t] = pl;
            }
            pl.docIds.push(d);
            pl.tfs.push(count);
        });
    }
    Atomics.add(counters, "tokensProcessed", toks.length);
    Atomics.add(counters, "docsIngested", 1);
}

function phaseANM(id) {
    if (nmScratch[id] === null)
        nmScratch[id] = makeNMScratch();
    const sc = nmScratch[id];
    for (;;) {
        const d = Atomics.add(counters, "nextDoc", 1);
        if (d >= N_BASE)
            break;
        ingestDocNM(d, sc);
    }
}

// checksumA over (termId, docId, tf) via mix32 — no string hash.
function postingsChecksumNM() {
    let sum = 0, postings = 0;
    for (let t = 0; t < V; ++t) {
        const pl = nmPost[t];
        if (pl === undefined) continue;
        const th = mix32(t);
        const docIds = pl.docIds, tfs = pl.tfs, n = docIds.length;
        postings += n;
        for (let i = 0; i < n; ++i) {
            const item = (th
                ^ ($imul(docIds[i], 0xd6e8feb9) >>> 0)
                ^ ($imul(tfs[i], 0xcaaf00dd) >>> 0)) >>> 0;
            sum = (sum + mix32(item)) | 0;
        }
    }
    return { sum: BigInt(sum >>> 0), postings };
}

function buildDfSnapNM() {
    const df = new Int32Array(V);
    for (let t = 0; t < V; ++t) {
        const pl = nmPost[t];
        if (pl !== undefined) df[t] = pl.docIds.length;
    }
    return df;
}

function copyPostingsNM(t) {
    const shard = shards[nmShardOf[t]];
    let docIds = null, tfs = null;
    shard.lock.hold(() => {
        const pl = nmPost[t];
        if (pl !== undefined) {
            docIds = pl.docIds.slice();
            tfs = pl.tfs.slice();
        }
    });
    return docIds === null ? { docIds: [], tfs: [] } : { docIds, tfs };
}

function queryPointNM(p) {
    const t = pickTermI(p);
    const shard = shards[nmShardOf[t]];
    let df = 0, sumTf = 0;
    shard.lock.hold(() => {
        const pl = nmPost[t];
        if (pl !== undefined) {
            for (let i = 0; i < pl.docIds.length; ++i) {
                if (pl.docIds[i] < N_BASE) { df++; sumTf += pl.tfs[i]; }
            }
        }
    });
    return mix32((mix32(t) ^ $imul(df, 0x9e37) ^ sumTf) >>> 0);
}

function queryAndNM(p) {
    const nTerms = 2 + (next32(p) % 2);
    const lists = [];
    for (let i = 0; i < nTerms; ++i)
        lists.push(copyPostingsNM(pickTermI(p)));
    const cand = new Map();
    const first = lists[0];
    for (let i = 0; i < first.docIds.length; ++i) {
        const d = first.docIds[i];
        if (d < N_BASE) cand.set(d, 1);
    }
    for (let k = 1; k < nTerms; ++k) {
        const list = lists[k];
        for (let i = 0; i < list.docIds.length; ++i) {
            const d = list.docIds[i];
            if (d < N_BASE && cand.get(d) === k) cand.set(d, k + 1);
        }
    }
    let matchSum = 0, matchCount = 0;
    for (const [d, c] of cand) {
        if (c === nTerms) { matchSum = (matchSum + mix32(d)) >>> 0; matchCount++; }
    }
    return (mix32(matchSum) ^ matchCount) >>> 0;
}

function queryScoredNM(p, dfSnap) {
    const nTerms = 2 + (next32(p) % 2);
    const cand = new Map();
    for (let k = 0; k < nTerms; ++k) {
        const t = pickTermI(p);
        const df = dfSnap[t];
        const idf = df === 0 ? 0 : ((N_BASE * 1000 / df) | 0);
        const list = copyPostingsNM(t);
        for (let i = 0; i < list.docIds.length; ++i) {
            const d = list.docIds[i];
            if (d < N_BASE) {
                const cur = cand.get(d);
                const add = $imul(list.tfs[i], idf) >>> 0;
                cand.set(d, cur === undefined ? add : (cur + add) >>> 0);
            }
        }
    }
    const entries = [];
    for (const [d, score] of cand)
        entries.push({ d, score });
    entries.sort((a, b) => {
        if (a.score !== b.score) return a.score > b.score ? -1 : 1;
        return a.d - b.d;
    });
    const top = entries.length < TOPN ? entries : entries.slice(0, TOPN);
    let h = 0x811c9dc5 >>> 0;
    for (let i = 0; i < top.length; ++i) h = fnvU32LE(h, top[i].d);
    for (let i = 0; i < top.length; ++i) h = fnvU32LE(h, top[i].score);
    return h;
}

function phaseBNM(id) {
    const sc = nmScratch[id];
    const dfSnap = published.dfSnap;
    let cs = 0;
    for (;;) {
        const q = Atomics.add(counters, "nextOp", 1);
        if (q >= N_QUERIES)
            break;
        if (q % WRITER_MOD === 0) {
            ingestDocNM(N_BASE + q / WRITER_MOD, sc);
            Atomics.add(counters, "writesDone", 1);
            continue;
        }
        const p = { s: qSeedI(q) };
        const kind = next32(p) % 10;
        let perQueryHash;
        if (kind <= 3)      perQueryHash = queryPointNM(p);
        else if (kind <= 7) perQueryHash = queryAndNM(p);
        else                perQueryHash = queryScoredNM(p, dfSnap);
        cs = (cs + mix32(($imul(q, GOLDEN32) ^ perQueryHash) >>> 0)) >>> 0;
        Atomics.add(counters, "queriesDone", 1);
    }
    partialBI[id] = cs;
}

// Phase C (nomap): claim s∈[0,K) via nextShard exactly like intcs; each s
// owns the contiguous termId block [s*512, (s+1)*512). Same Atomics counter
// traffic, same group-lock fan-in. Group key derived arithmetically from
// termId (no termOf string); per-group terms list stores termId ints.
function phaseCNM() {
    const groups = published.groups;
    const block = V / K; // 512
    for (;;) {
        const s = Atomics.add(counters, "nextShard", 1);
        if (s >= K) break;
        const lo = s * block, hi = lo + block;
        for (let t = lo; t < hi; ++t) {
            const pl = nmPost[t];
            if (pl === undefined) continue;
            let totalTf = 0;
            const df = pl.docIds.length;
            for (let i = 0; i < df; ++i) totalTf += pl.tfs[i];
            const gk = (termLenBucket(t) - 2) * 26 + firstLetterIdx(t);
            const g = groups[gk];
            g.lock.hold(() => {
                g.totalTf += totalTf;
                g.df += df;
                g.termIds.push(t);
                g.termTfs.push(totalTf);
            });
        }
    }
}

function makeGroupsNM() {
    const groups = [];
    for (let g = 0; g < 104; ++g)
        groups.push({ lock: new Lock(), totalTf: 0, df: 0, termIds: [], termTfs: [] });
    return groups;
}

function checksumPhaseCNM() {
    const groups = published.groups;
    let sum = 0;
    for (let gk = 0; gk < 104; ++gk) {
        const g = groups[gk];
        const ids = g.termIds, tfv = g.termTfs, n = ids.length;
        // Full sort by (totalTf DESC, termId ASC) — matches Go sort.Slice /
        // Java ArrayList.sort, so the topN slice is identical across langs.
        const idx = [];
        for (let i = 0; i < n; ++i) idx.push(i);
        idx.sort((a, b) => {
            if (tfv[a] !== tfv[b]) return tfv[b] - tfv[a];
            return ids[a] - ids[b];
        });
        const top = n < TOPN ? n : TOPN;
        // termOf(id) only here — the one place a string is genuinely needed.
        let joined = "";
        for (let i = 0; i < top; ++i)
            joined += termOf(ids[idx[i]]) + ":" + tfv[idx[i]] + ",";
        const len = (gk / 26 | 0) + 2;
        const key = len + ":" + String.fromCharCode(A_CODE + (gk % 26));
        const item = (fnv1a32(key)
            ^ g.totalTf
            ^ ($imul(g.df, GOLDEN32) >>> 0)
            ^ fnv1a32(joined)) >>> 0;
        sum = (sum + mix32(item)) >>> 0;
    }
    return sum;
}

function checksumPhaseCI() {
    const groups = published.groups;
    const keys = published.groupKeys;
    let sum = 0;
    for (let i = 0; i < keys.length; ++i) {
        const key = keys[i];
        const g = groups.get(key);
        g.terms.sort((a, b) => {
            if (a.totalTf !== b.totalTf)
                return b.totalTf - a.totalTf;
            return a.term < b.term ? -1 : (a.term > b.term ? 1 : 0);
        });
        const top = g.terms.length < TOPN ? g.terms : g.terms.slice(0, TOPN);
        let joined = "";
        for (let j = 0; j < top.length; ++j)
            joined += top[j].term + ":" + top[j].totalTf + ",";
        const item = (fnv1a32(key)
            ^ g.totalTf
            ^ ($imul(g.df, GOLDEN32) >>> 0)
            ^ fnv1a32(joined)) >>> 0;
        sum = (sum + mix32(item)) >>> 0;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// §1.10-WS Phase C — WORK-STEALING ARM (mode-selected; the naive phaseC()
// above is untouched — it is the lock-pathology diagnostic). Selected by the
// literal extra shell argument "ws" (jsc has no env accessor; same mechanism
// as the "smoke" argument): jsc <flags> bench.js -- W [smoke] ws
//
// Algorithm (identical in go/main.go and Bench.java):
//  - W per-worker deques over the 128 shard indices; shard s seeded into
//    deque s % W in increasing s order. Each deque is a fixed array with
//    [head, tail) live region, protected by the one mutex type (Lock —
//    fairness rule §2.1; no lock-free Chase-Lev cleverness in any language).
//  - Owner pops from the TAIL of its own deque; when empty it scans victims
//    (w+1 .. w+W-1 mod W) and steals ceil(n/2) items from the victim's HEAD
//    (victim lock held for the copy only, then own lock to append — never
//    nested, so no deadlock).
//  - Termination: shared atomic wsRemaining (init K) decremented per popped
//    shard; a worker exits when its deque is empty, a full steal scan fails,
//    and wsRemaining == 0 (otherwise re-scan: items can be transiently
//    in-flight inside a steal).
//  - Accumulation is THREAD-LOCAL: each worker folds its shards into a local
//    map groupKey -> { totalTf, df, terms[] } (no group locks taken during
//    the phase). After the Phase C barrier, thread 0 merges the W local maps
//    into the shared groups single-threaded, then the naive sort/topN/
//    checksumC code runs unchanged. checksumC is an order-independent
//    mod-2^64 sum and each term occurs in exactly one shard, so the merged
//    result is bit-identical to the naive arm's.
// ---------------------------------------------------------------------------
const WS_MODE = shellArgs.indexOf("ws") >= 0;

const wsDeques = [];   // shared objects; each { lock, arr, head, tail }
const wsLocals = [];   // slot per worker for its thread-local accumulator
const wsState = { remaining: 0 };
if (WS_MODE) {
    for (let w = 0; w < W; ++w) {
        wsDeques.push({ lock: new Lock(), arr: [], head: 0, tail: 0 });
        wsLocals.push(null);
    }
    for (let s = 0; s < K; ++s) {
        const dq = wsDeques[s % W];
        dq.arr[dq.tail++] = s;
    }
    wsState.remaining = K;
}

function wsPopOwn(w) {
    const dq = wsDeques[w];
    let item = -1;
    dq.lock.hold(() => {
        if (dq.head < dq.tail)
            item = dq.arr[--dq.tail];
    });
    return item;
}

function wsSteal(w) {
    for (let off = 1; off < W; ++off) {
        const dq = wsDeques[(w + off) % W];
        let stolen = null;
        dq.lock.hold(() => {
            const n = dq.tail - dq.head;
            if (n > 0) {
                const cnt = (n + 1) >> 1; // steal-half = ceil(n/2), from the head
                stolen = dq.arr.slice(dq.head, dq.head + cnt);
                dq.head += cnt;
            }
        });
        if (stolen !== null) {
            const own = wsDeques[w];
            own.lock.hold(() => {
                for (let i = 0; i < stolen.length; ++i)
                    own.arr[own.tail++] = stolen[i];
            });
            return true;
        }
    }
    return false;
}

function phaseCWS(id) {
    const local = new Map(); // thread-local accumulator: key -> {totalTf, df, terms}
    for (;;) {
        const s = wsPopOwn(id);
        if (s < 0) {
            if (wsSteal(id))
                continue;
            if (Atomics.add(wsState, "remaining", 0) <= 0) // atomic load
                break;
            continue; // transiently in-flight steal; re-scan
        }
        Atomics.add(wsState, "remaining", -1);
        for (const [term, p] of shards[s].map) {
            let totalTf = 0;
            const df = p.docIds.length; // ALL postings: base + writer docs
            for (let i = 0; i < df; ++i)
                totalTf += p.tfs[i];
            const key = term.length + ":" + term.charAt(1); // same key rule as phaseC()
            let g = local.get(key);
            if (g === undefined) {
                g = { totalTf: 0, df: 0, terms: [] };
                local.set(key, g);
            }
            g.totalTf += totalTf;
            g.df += df;
            g.terms.push({ term, totalTf });
        }
    }
    wsLocals[id] = local;
}

// Thread 0, single-threaded after the Phase C barrier: merge the W local
// accumulators into the shared groups, then checksumPhaseC() runs unchanged.
function wsMergeLocals() {
    const groups = published.groups;
    for (let w = 0; w < W; ++w) {
        const local = wsLocals[w];
        for (const [key, lg] of local) {
            const g = groups.get(key);
            g.totalTf += lg.totalTf;
            g.df += lg.df;
            for (let i = 0; i < lg.terms.length; ++i)
                g.terms.push(lg.terms[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// FLAT ARM — §34 discriminant. Everything below is gated on FLAT; when FLAT
// is false none of this is reachable and the spec-exact path runs unchanged.
// Concurrency surface kept IDENTICAL: same shards[i].lock objects, same
// barrier, same Atomics counters, same K/W/N_BASE/WRITER_MOD/TOPN, same
// worker-body barrier structure. Only the data representation changes.
// ---------------------------------------------------------------------------

// --- 32-bit Number-only PRNG (splitmix32; zero BigInt). p is {s: u32}.
function next32(p) {
    p.s = (p.s + 0x9e3779b9) >>> 0;
    let z = p.s;
    z = $imul(z ^ (z >>> 16), 0x85ebca6b) >>> 0;
    z = $imul(z ^ (z >>> 13), 0xc2b2ae35) >>> 0;
    return (z ^ (z >>> 16)) >>> 0;
}
function docSeed32(d) { return mix32(0x5ca1ab1e ^ ($imul(d, 0x9e3779b9) >>> 0)); }
function qSeed32(q)   { return mix32(0xfacefeed ^ ($imul(q, 0x9e3779b9) >>> 0)); }
function pickTermFlat(p) {
    const a = next32(p) & (V - 1);
    const b = next32(p) & (V - 1);
    return a < b ? a : b;
}
// term string length bucket (2..5) without building the string: "t"+base26(i).
function termLenBucket(t) { return t < 26 ? 2 : t < 676 ? 3 : t < 17576 ? 4 : 5; }

// --- Per-shard flat storage. ORIGINAL DESIGN was a linked-list-in-Int32Arrays
// (heads[512] per shard, push = 4 indexed stores + n++). That hit a genuine
// engine race under GIL-off: ~1-4 in 12 W=4 runs dropped 30-480 postings.
// Bisection log (2026-06-15): persisted across SAB vs non-SAB backing, across
// pre-sized cap (no grow path), across plain-Array backing with .push(), and
// across per-thread vs main-thread scratch allocation; rawN (Σ docIds.length)
// always == chain-walk count, so the loss is upstream of storage; adding
// Atomics.add fences in the push loop did NOT eliminate it. The default arm's
// identically-shaped lock.hold + Array.push is deterministic on the same
// build, so the trigger is specific to the int-only / typed-array-heavy hot
// loop (likely a JIT tier interaction with the per-worker Int32Array tf/dirty
// scratch under GIL-off). RECORD in SCALEBENCH.md §34 as an engine finding;
// out of scope to fix here.
//
// ADOPTED DESIGN: per-shard `Map<int,{docIds:[],tfs:[]}>` — the default arm's
// proven shape verbatim, keyed by termId int instead of term string. The
// per-doc tf accumulator is also a fresh `Map<int,int>` (default-arm shape).
// This is correct by the default-arm checksum gate and still removes 100% of
// the string/BigInt/tokenize/fnv1a cost from the hot path, which is the
// discriminant the flat arm exists to measure.
function makeFlatShard() { return new Map(); }

// --- Per-worker scratch (allocated ON the worker thread; reset between
// uses). Sized to N_BASE for the docId-indexed maps.
//
// hold-closure-alloc (SCALEBENCH §34 flat-gap): every flat-arm
// `lock.hold(() => …)` allocates a fresh JSFunction + JSLexicalEnvironment per
// call (the arrow captures loop-varying t/d/count/fmap/…). At W=16 perf shows
// allocateCell<JSFunction> 2.68% + operationCreateLexicalEnvironmentTDZ 2.57%
// + operationNewArrowFunction 0.87% = 6.12% on-CPU, plus a share of
// writeBarrierSlowPath 6.6% and CompleteSubspace::allocateForClient 2.0%;
// logGC accounts ~300 MB of the ~600 MB total run allocation to these
// throwaway closures, driving ~half the GC cycles. Go/Java allocate ZERO
// bytes per critical section (mu.Lock(); body; mu.Unlock()). DFG cannot sink
// the allocation (the closure escapes into a native call), and the JS Lock
// API has no lock()/unlock() pair. Hoist instead: one JSFunction per
// (worker × call-site), captured over `sc` only, with the loop-varying values
// staged through `sc.h*` slots immediately before the (synchronous) hold()
// call. `sc` is per-worker thread-local, hold() runs the body to completion
// before returning, so the slot writes never race. The closure bodies are
// byte-for-byte the inline arrows, so the flat-arm checksum reference
// 686d6890|4154468|0fbbd673|3af6b072|e1d22021 is unchanged. Total flat-arm
// closure+env allocations: ~5M → 5×W. The hold-vmEntry-trampoline fast path
// (LockObject.cpp) keys on FunctionExecutable->codeBlockForCall(), which is
// now warm after the FIRST call instead of re-walking a fresh JSFunction each
// time — same fast-path predicate, same 0-arg shape.
function makeFlatScratch() {
    const sc = {
        // go-gap-liveheap-7x-mark-15pct-postA: per-doc termId-indexed tf
        // accumulator + dirty list (replaces the per-doc `new Map()` in
        // ingestDocFlat / ingestWriterDocFlat — see those callsites). Reset is
        // dirty-list-only (≤213 stores), so the 256 KB V-sized scratch is
        // touched proportionally to nTok, not V. Per-worker thread-local;
        // never read across the shard lock.
        tfScratch: new Int32Array(V),
        tfDirty: new Int32Array(256), // max unique terms/doc = titleLen+bodyLen ≤ 13+200
        // phaseB: docId-indexed candidate count/score + dirty list
        candCount: new Int32Array(N_BASE),
        candScore: new Float64Array(N_BASE),
        candDirty: new Int32Array(N_BASE),
        // phaseB scored: top-N selection scratch
        sortD: new Int32Array(N_BASE),
        sortS: new Float64Array(N_BASE),
        prng: { s: 0 },
        // hold-closure-alloc: per-call-site capture slots (written
        // immediately before hold(), read inside the hoisted body).
        hT: 0, hD: 0, hCount: 0, hFmap: null, hG: null, hTotalTf: 0, hDf: 0,
        hOutDocIds: null, hOutTfs: null, hOutDf: 0, hOutSumTf: 0,
        ingestHold: null, ingestWriterHold: null,
        copyPostingsHold: null, queryPointHold: null, phaseCHold: null,
    };
    sc.ingestHold = function() {
        // phaseA-ingestHold-born-int32array-kills-segpush (FLAT-GAP-EVIDENCE
        // §37 round 4): the previous JSArray `pl.docIds.push / pl.tfs.push`
        // here was THE 15.9% W=16 segmented-push family — 4.68M holds × 2 =
        // 9.36M [].push onto foreign-thread-segmented JSArrays (perf:
        // ButterflySpine::publicLength 8.17% + bumpPublicLengthToAtLeast 3.40%
        // + JSArray::pushInline 2.51% + operationArrayPush 0.81% +
        // tryGrowSegmentedVectorLength 0.62%). Pre-sizing is infeasible (per-
        // term posting count is the Zipf draw outcome, 1..~7k). But
        // ingestWriterHold below already carries the right shape: geometric-
        // doubling Int32Array appends under the same shard lock, born typed.
        // Adopting that body verbatim removes ALL 9.36M segmented pushes,
        // collapses flatten1 to the trim-slice-only arm (len is never 0, no
        // `new Int32Array(segmentedJSArray)` source), and removes the W=32
        // bc#223 BadIndexingType speculation surface entirely (Int32Array has
        // exactly one DFG ArrayMode). Go's []int append / Java's
        // ArrayList.add are amortized-O(1) typed appends → fairness-neutral.
        // Same {docIds,tfs,len} inlineCapacity=3 literal as ingestWriterHold,
        // so the pl wrapper stays Structure-monomorphic process-wide. Data
        // identical (same per-term append order under the shard lock) → flat
        // checksum reference 686d6890|4154468|0fbbd673|3af6b072|e1d22021
        // unchanged. Direct A/B @94fb0ed54cf6 3-rep med: W=16 phaseA 461→350
        // (−24%) flatten1 71→50 total 823→706 (−14%); W=32 10-rep phaseA
        // slow-mode 2/10→0/10 total med 840→633 (−25%).
        const t = sc.hT, fmap = sc.hFmap, d = sc.hD, count = sc.hCount;
        const pl = fmap.get(t);
        if (pl === undefined) {
            const nd = new Int32Array(4), nt = new Int32Array(4);
            nd[0] = d; nt[0] = count;
            fmap.set(t, { docIds: nd, tfs: nt, len: 1 });
            return;
        }
        let len = pl.len, docIds = pl.docIds, tfs = pl.tfs;
        if (len === docIds.length) {
            const cap = len << 1;
            const nd = new Int32Array(cap); nd.set(docIds); pl.docIds = docIds = nd;
            const nt = new Int32Array(cap); nt.set(tfs);    pl.tfs    = tfs    = nt;
        }
        docIds[len] = d; tfs[len] = count;
        pl.len = len + 1;
    };
    sc.ingestWriterHold = function() {
        const t = sc.hT, fmap = sc.hFmap, d = sc.hD, count = sc.hCount;
        const pl = fmap.get(t);
        if (pl === undefined) {
            const nd = new Int32Array(4), nt = new Int32Array(4);
            nd[0] = d; nt[0] = count;
            fmap.set(t, { docIds: nd, tfs: nt, len: 1 });
            return;
        }
        let len = pl.len, docIds = pl.docIds, tfs = pl.tfs;
        if (len === docIds.length) {
            const cap = len << 1;
            const nd = new Int32Array(cap); nd.set(docIds); pl.docIds = docIds = nd;
            const nt = new Int32Array(cap); nt.set(tfs);    pl.tfs    = tfs    = nt;
        }
        docIds[len] = d; tfs[len] = count;
        pl.len = len + 1;
    };
    sc.copyPostingsHold = function() {
        const pl = sc.hFmap.get(sc.hT);
        if (pl !== undefined) {
            const len = pl.len;
            sc.hOutDocIds = pl.docIds.slice(0, len);
            sc.hOutTfs = pl.tfs.slice(0, len);
        }
    };
    sc.queryPointHold = function() {
        const pl = sc.hFmap.get(sc.hT);
        let df = 0, sumTf = 0;
        if (pl !== undefined) {
            const docIds = pl.docIds, tfs = pl.tfs, n = pl.len;
            for (let i = 0; i < n; ++i)
                if (docIds[i] < N_BASE) { df++; sumTf += tfs[i]; }
        }
        sc.hOutDf = df; sc.hOutSumTf = sumTf;
    };
    sc.phaseCHold = function() {
        const g = sc.hG;
        g.totalTf += sc.hTotalTf;
        g.df += sc.hDf;
        g.termIds.push(sc.hT);
        g.termTfs.push(sc.hTotalTf);
    };
    return sc;
}

const flatShards = [];
const flatScratch = [];          // slot per worker; allocated ON the worker thread
const partialBFlat = new Array(W).fill(0);
if (FLAT) {
    for (let s = 0; s < K; ++s)
        flatShards.push(makeFlatShard());
    for (let w = 0; w < W; ++w)
        flatScratch.push(null);
}

// --- Phase A (flat)
// go-gap-liveheap-7x-mark-15pct-postA: the previous per-doc `new Map()` tf
// accumulator allocated 28k JSMap shells + ~2.8M HashMapBucket cells + 28k
// JSCellButterfly bucket vectors over phaseA (and 2.8k more in the phaseB
// writer). Under W=16 GIL-off the eden collector promotes most of those into
// old-gen before the doc loop drops them, so they survive as carcasses until
// the next FullCollection — logGC W=16 reports the post-phaseA live set at
// ~645 MB vs Go's ~86 MB (gctrace gc 39: 161→86 MB), a 7.5× bloat, and
// perf -D 700 W=16 attributes 9.68% self to JSCellButterfly::visitChildrenImpl
// (Map bucket-vector marking) + 3.19% SlotVisitor::markAuxiliary + 1.27%
// drain + 1.14% visitSegmentedButterfly = 15.28% on-CPU in mark, plus ~3
// FullCollection cycles at 29 ms each. Go's per-doc `map[int]int` is a single
// flat hmap struct the tracer walks in O(buckets) with no per-entry cell;
// Java's HashMap<Integer,Integer> entries are nursery-local and die before
// promotion under G1's region budget. The flat arm's whole point is "if
// structs were implemented" — a per-worker reusable Int32Array(V) tf scratch
// + Int32Array dirty list IS the struct-shaped accumulator (Go's map ≈ open-
// addressed int array). Iteration below walks tfDirty in first-occurrence
// order, which is exactly Map insertion order, so the per-term shard-lock
// sequence and posting-list contents are byte-identical and the flat-arm
// checksum reference 686d6890|4154468|0fbbd673|3af6b072|e1d22021 is
// unchanged. Per-doc allocation: 1 JSMap + ~100 buckets + 1 bucket vector +
// 1 MapIterator → 0. Reset cost: ≤213 indexed int32 stores.
function ingestDocFlat(d, sc) {
    const p = sc.prng; p.s = docSeed32(d);
    const titleLen = 5 + (next32(p) % 8);
    const bodyLen = 80 + (next32(p) % 121);
    const nTok = titleLen + bodyLen;
    const tf = sc.tfScratch, dirty = sc.tfDirty;
    let nDirty = 0;
    for (let j = 0; j < nTok; ++j) {
        const t = pickTermFlat(p);
        if (tf[t] === 0) dirty[nDirty++] = t;
        tf[t]++;
    }
    // hold-closure-alloc: stage captures through sc, reuse the per-worker
    // hoisted body (see makeFlatScratch). hD is loop-invariant per doc.
    // sc.ingestHold's body is the born-Int32Array geometric-grow append (see
    // the round-4 provenance comment at its definition in makeFlatScratch);
    // phaseA keeps its OWN function identity here (separate from phaseB's
    // sc.ingestWriterHold, byte-identical body) so DFG/FTL profile the two
    // call shapes independently — phaseA sees only fresh-term first-inserts +
    // small lists, phaseB-writer sees only post-flatten warm lists.
    sc.hD = d;
    const ingestHold = sc.ingestHold;
    for (let k = 0; k < nDirty; ++k) {
        const t = dirty[k];
        const s = t & (K - 1);
        sc.hT = t; sc.hCount = tf[t]; sc.hFmap = flatShards[s];
        shards[s].lock.hold(ingestHold);
        tf[t] = 0; // reset-on-consume: scratch is all-zero on return
    }
    Atomics.add(counters, "tokensProcessed", nTok);
    Atomics.add(counters, "docsIngested", 1);
}

function phaseAFlat(id) {
    // Allocate per-worker scratch HERE (on the worker thread) so the
    // typed-array buffers are truly thread-local.
    if (flatScratch[id] === null)
        flatScratch[id] = makeFlatScratch();
    const sc = flatScratch[id];
    for (;;) {
        const d = Atomics.add(counters, "nextDoc", 1);
        if (d >= N_BASE)
            break;
        ingestDocFlat(d, sc);
    }
}

// serial-seg-getbutterfly-osrexit / serial-section-tier-pollution: at W>1 the
// posting-list JSArrays were `[]`-allocated by one worker and `.push()`-ed by
// foreign workers under the shard lock in ingestDocFlat. Two compounding JSC
// representation artefacts follow, neither present in Go's []int / Java's
// ArrayList (contiguous regardless of which goroutine/thread appended under
// the mutex), so normalizing them is a fairness correction:
//
//   (a) ~94% carry a SEGMENTED butterfly (SPEC-objectmodel §4.2). FTL
//       compileGetButterfly (FTLLowerDFGToB3.cpp) has no inline segmented-read
//       arm and speculate-exits BadIndexingType on the tagged word, so a flat
//       copy is required for the serial readers to stay in FTL at all.
//
//   (b) Even after (a), the `.slice()` copies inherit the SOURCE indexing
//       shape (JSArray::fastSlice allocates the result with the source's
//       resolved IndexingType). At W=16 the cross-thread fill leaves a MIX of
//       observed shapes in the ArrayProfile for `docIds[i]` / `tfs[i]`, so
//       DFGArrayMode::fromObserved falls into its multi-shape default arm,
//       postingsChecksumFlat CheckArray-exits, and verboseOSR shows
//       Baseline→DFG→Baseline→DFG→Baseline→DFG (3 DFG installs) before FTL
//       finally settles — pc1 1090 ms vs pc2 756 ms (334 ms warmup) and pc2
//       still 1.36× the W=1 baseline. At W=1 every array was filled by one
//       thread → uniform ArrayWithInt32 → straight-line tier-up, pc1≈pc2≈550.
//       That delta is 100% on the wall-clock critical path (15 threads parked
//       at the barrier).
//
// Normalize HARD: rebuild each posting list as a fresh thread-0-allocated
// `{docIds, tfs}` pair backed by `Int32Array`. An Int32Array has exactly one
// possible DFG ArrayMode (Array::Int32Array, AsIs) and one global structure —
// the serial readers (postingsChecksumFlat, buildDfSnapFlat, phaseCFlat's
// totalTf sum) and the phaseB read paths (queryPointFlat, copyPostingsFlat,
// queryAndFlat, queryScoredFlat — all `.length`/`[i]`/`.slice()` only) are
// then monomorphic by construction at every W. The wrapper object is also
// rebuilt fresh on thread 0 so `pl.docIds` GetById sees a single never-
// foreign-written structure. `new Int32Array(jsArray)` takes the
// isIteratorProtocolFastAndNonObservable C++ fast path (indexed copy, no
// iterator-result allocs), and `new Int32Array(int32Array)` (second flatten)
// is a same-type memcpy. Values are small non-negative ints (docId <
// N_BASE+N_QUERIES/WRITER_MOD, tf < 256), so the int32 cast is identity and
// the flat-arm checksum reference 686d6890|4154468|0fbbd673|3af6b072|e1d22021
// is unchanged at every W. At W=1 this is a flat-JSArray→Int32Array copy of
// ~8.3M ints, ≲1-2% of the W=1 wall.
// flatten1-parallelize-shards-out-of-pc1-timer (§36): the K=128 shard Maps
// are independent (no cross-shard reads, only data published at the phaseA
// barrier), so this is embarrassingly shard-parallel. Go/Java have NO flatten
// step at all — their []int / ArrayList<Integer> are contiguous regardless of
// which goroutine/thread appended under the mutex — so timing a JS-only
// segmented→typed normalization INSIDE postingsChecksum1_ms is a fairness
// defect against the cross-language metric. The first flatten runs as a
// W-parallel section (worker `id` owns shards where `s % W === id`; each
// shard Map is read+rewritten by exactly one worker during the section)
// between the phaseA barrier and a new barrier, OUTSIDE the pc1 timer. The
// `new Int32Array(jsArray)` source is the SHARED segmented-butterfly posting
// list (foreign-thread pushes under the shard lock); the §36 engine-side
// flatten1-segmented-int32shape-segwalk-copy fix makes that copy a fragment-
// run walk instead of the per-element PropertySlot chain, so the per-shard
// cost is now flat-W. The wrapper object and its Int32Array fields share one
// Structure / one ArrayMode regardless of allocating worker (same source
// location, shared global object, shared heap), so the serial readers stay
// monomorphic. Second flatten (post-phaseB) stays thread-0 serial: sources
// are already Int32Array (same-type memcpy, ~65 ms) and fall under pc2.
function flattenFlatShards(first, stride) {
    // flatten1-full-FASTER-than-noTA-proves-collision (FLAT-GAP-EVIDENCE §36
    // round 3 discriminant): at W=16 the FULL body below previously ran in
    // 47-54ms but a noTA probe (objlit-only, NO `new Int32Array`) ran in
    // 80ms — ADDING ~131k typed-array constructions made the loop ~30ms
    // FASTER because the createUninitialized+segwalk work spaced out the
    // acquisitions of the global empty-`{}` Structure's m_lock and halved
    // the collision rate. Canonical signature of a collision-bound (not
    // work-bound) lock; the contended site is the `{docIds,tfs,len:}`
    // literal's first transition empty→{docIds}: the empty-object Structure
    // has many first-property outgoing edges program-wide, so its transition
    // table is map-tagged and the round-3 single-slot DCLP precheck in
    // addPropertyTransitionToExistingStructureConcurrently (Structure.cpp)
    // falls through to m_lock on every one of ~65k literals × W workers.
    // FIX: drop the fresh wrapper entirely and mutate the existing `pl` in
    // place — the Map already holds it, so no fmap.set either. The two field
    // replaces are existing-slot writes (per-object, no transition); the one
    // `.len` add transitions the {docIds,tfs} Structure, whose ONLY outgoing
    // edge is →len (every site that grows this shape adds `len` next), so it
    // is single-slot and the precheck resolves it lock-free. Net m_lock
    // traffic from flatten1: zero. Bound: flatten1 W=16 collapses to the
    // typed-array work floor ≈ (full W=1 91ms − noTA W=1 21ms)/16 + per-
    // worker FTL warmup ≈ 13-15ms. The "rebuild wrapper fresh on thread 0"
    // rationale above is stale anyway (flatten1 is W-parallel now); the
    // phaseA ingestHold and phaseB ingestWriterHold `{docIds,tfs,len:1}`
    // literals share the SAME inlineCapacity=3 Structure,
    // and the in-place writes here are existing-slot replaces (no transition),
    // so downstream `pl.docIds` GetById stays monomorphic. (A 2-prop phaseA
    // literal would NOT — its later +len transitions to a distinct OOL-len
    // Structure and postingsChecksumFlat OSR-exits BadCache; §37 measurement
    // caught this.) Data identical → flat checksum
    // reference 686d6890|4154468|0fbbd673|3af6b072|e1d22021 unchanged.
    for (let s = first; s < K; s += stride) {
        const fmap = flatShards[s];
        for (const pl of fmap.values()) {
            const len = pl.len;
            // Post-round-4 (phaseA-ingestHold-born-int32array-kills-segpush)
            // ingestHold is born-typed with len≥1, so the len===0 JSArray-
            // source arm below is DEAD — both flatten passes uniformly take
            // the Int32Array→Int32Array .slice(0,len) trim arm. The ternary
            // is retained as a harmless guard; DFG profiles it never-taken.
            const docIds = len === 0
                ? new Int32Array(pl.docIds) : pl.docIds.slice(0, len);
            const tfs = len === 0
                ? new Int32Array(pl.tfs) : pl.tfs.slice(0, len);
            // flatten1-knee-is-pl-putbyid-writeback-NOT-newInt (FLAT-GAP-
            // EVIDENCE round-4 §(3b)): the W>8 flatten1 knee is THESE THREE
            // PutByIds, not the typed-array construction above. Discriminants
            // on 94fb0ed5: (a) noTA (drop ctor, keep puts) → full knee
            // {22,73,94}ms@W={8,16,32}; (b) sum-sink (drop puts, keep reads
            // live) → flat {10,10,10}ms; (c) fixed-64 (new Int32Array(64),
            // keep puts) → full knee {30,72,88}ms. uprobe W=16: putgaveup=0,
            // putDirectInternalConcurrentOutOfLine=165-259, llint put_by_id
            // 1.6-5.6k, STW 433 vs sum-sink 491 — i.e. all ~196k puts stay on
            // the JIT IC fast path; the knee is inside the inline replace-
            // store on a foreign-thread-allocated butterfly. REFUTES round-4
            // §(3)'s setButterflyConcurrent/findConcurrently attribution (its
            // /tmp/r4/bench-flatperf.js perf-slice had NO store-back, so it
            // profiled the wrong loop) and the round-3 "collapses to ~13-15ms
            // typed-array work floor" bound above. Redirects R4 finder budget
            // off TA-ctor/Gigacage/iterator-protocol onto the PutById-replace
            // IC fast path under W>8 cross-thread ownership. The puts are
            // load-bearing for the flat checksum so cannot be dropped here.
            pl.docIds = docIds;
            pl.tfs = tfs;
            pl.len = docIds.length;
        }
    }
}

// Phase-B writer ingest (post-flatten). Body matches ingestDocFlat exactly
// except for the per-term append: the posting lists are now Int32Array (no
// `.push`), so use Java's PostingList shape — geometric-doubling Int32Array
// with a separate `.len` field — for amortized-O(1) appends INSIDE the held
// shard lock (Go `append`/Java `pl.add` are amortized-O(1) in the same
// critical section; the previous grow-by-one `appendI32` was O(n) per push:
// ≈280k typed-array constructions + ~72 MB Gigacage memcpy under the lock,
// which serialized phaseB scaling). Every list typed-monomorphic for the
// second flatten / pc2 / phaseC; new terms (pl===undefined) are also born as
// Int32Array so pc2 never sees a mix. flatten1 stamps `.len` on every pl so
// the phaseB readers (queryPointHold/copyPostingsHold) and ingestWriterHold
// see a uniform 3-prop Structure. Kept as a SEPARATE function so the phaseA
// hot path (ingestDocFlat) stays byte-identical and its JIT profile is not
// polluted by the typed-array arm.
function ingestWriterDocFlat(d, sc) {
    const p = sc.prng; p.s = docSeed32(d);
    const titleLen = 5 + (next32(p) % 8);
    const bodyLen = 80 + (next32(p) % 121);
    const nTok = titleLen + bodyLen;
    // go-gap-liveheap-7x-mark-15pct-postA: same per-worker tf scratch as
    // ingestDocFlat (see that comment). The phaseB writer fires 2800× and was
    // the only remaining `new Map()` on the flat-arm hot path; with it gone
    // the steady-state phaseB allocation profile is the copyPostingsFlat
    // slice pair + the ingestWriterHold geometric-grow buffers and nothing
    // else, so eden cycles stop promoting Map carcasses into the FullCollect
    // mark set. tfScratch is per-worker thread-local and reset-on-consume
    // below, so phaseA's last doc left it all-zero.
    const tf = sc.tfScratch, dirty = sc.tfDirty;
    let nDirty = 0;
    for (let j = 0; j < nTok; ++j) {
        const t = pickTermFlat(p);
        if (tf[t] === 0) dirty[nDirty++] = t;
        tf[t]++;
    }
    // hold-closure-alloc: see makeFlatScratch.
    sc.hD = d;
    const ingestWriterHold = sc.ingestWriterHold;
    for (let k = 0; k < nDirty; ++k) {
        const t = dirty[k];
        const s = t & (K - 1);
        sc.hT = t; sc.hCount = tf[t]; sc.hFmap = flatShards[s];
        shards[s].lock.hold(ingestWriterHold);
        tf[t] = 0;
    }
    Atomics.add(counters, "tokensProcessed", nTok);
    Atomics.add(counters, "docsIngested", 1);
}

// Same disease, same cure for the Phase-C group arrays: g.termIds/termTfs are
// pushed by all W workers in phaseCFlat → segmented + mixed shape;
// checksumPhaseCFlat's partial-selection loop both reads AND swap-writes them.
// Int32Array gives a single ArrayMode for both the read and the in-place swap
// (owner-path fast write on a thread-0-local typed buffer). termId < V=65536,
// termTfs is Σtf over one posting list (< ~2e5) — both well inside int32.
function flattenGroupsFlat() {
    const groups = published.groups;
    for (let gk = 0; gk < 104; ++gk) {
        const g = groups[gk];
        g.termIds = new Int32Array(g.termIds);
        g.termTfs = new Int32Array(g.termTfs);
    }
}

// Order-independent 32-bit postings checksum over the flat shards.
// `sEnd` is a plain (non-default) param so the bytecode keeps a simple
// parameter list — real callers pass K; the cs1 pre-warm passes a small
// prefix (see workerBody).
function postingsChecksumFlat(sEnd) {
    let sum = 0, postings = 0;
    for (let s = 0; s < sEnd; ++s) {
        for (const [t, pl] of flatShards[s]) {
            const th = mix32(t);
            // Hoist (matches queryPointFlat/phaseCFlat shape): under the
            // GIL-off CheckTraps widening (DFGClobberize.h:812+, see the $imul
            // comment) LICM cannot hoist the per-iteration `pl.docIds` GetById
            // across the loop header, so do it lexically.
            const docIds = pl.docIds, tfs = pl.tfs, n = docIds.length;
            postings += n;
            for (let i = 0; i < n; ++i) {
                const item = (th
                    ^ ($imul(docIds[i], 0xd6e8feb9) >>> 0)
                    ^ ($imul(tfs[i], 0xcaaf00dd) >>> 0)) >>> 0;
                // cs1-sum-ushr0-to-bitor0 (§37 R3): `>>> 0` emits op_unsigned
                // → UInt32ToNumber; SetLocal(sum) force-ORs UsesAsNumber onto
                // its child (DFGBackwardsPropagationPhase.cpp:291) and
                // UInt32ToNumber passes it through (424-427), so DFG#1 keeps
                // a checked UInt32ToNumber that Overflow-exits at bc#363 once
                // bit 31 sets; the now-Double `sum` makes prepareOSREntry
                // refuse bc#264 and we eat 3 DFG installs + 2 reopt rounds
                // before FTL. `| 0` emits no op_unsigned (ArithBitOr is the
                // SetLocal child, always Int32) — bits identical mod 2^32;
                // coerce back to uint32 once at return.
                // W=16 5-rep: cs1 med 67→35 ms, cs2 20→17, total 816→785;
                // checksumA/A2 unchanged (686d6890 / 0fbbd673).
                sum = (sum + mix32(item)) | 0;
            }
        }
    }
    return { sum: sum >>> 0, postings };
}

function buildDfSnapFlat() {
    const df = new Int32Array(V);
    for (let s = 0; s < K; ++s)
        for (const [t, pl] of flatShards[s])
            df[t] = pl.docIds.length;
    return df;
}

// --- Phase B (flat)
function copyPostingsFlat(t, sc) { // copy posting list under shard lock (default-arm shape)
    const s = t & (K - 1);
    // hold-closure-alloc: see makeFlatScratch.
    sc.hT = t; sc.hFmap = flatShards[s]; sc.hOutDocIds = null; sc.hOutTfs = null;
    shards[s].lock.hold(sc.copyPostingsHold);
    const docIds = sc.hOutDocIds;
    return docIds === null ? null : { docIds, tfs: sc.hOutTfs };
}

function queryPointFlat(p, sc) {
    const t = pickTermFlat(p);
    const s = t & (K - 1);
    // hold-closure-alloc: see makeFlatScratch.
    sc.hT = t; sc.hFmap = flatShards[s];
    shards[s].lock.hold(sc.queryPointHold);
    return mix32((mix32(t) ^ ($imul(sc.hOutDf, 0x9e37) >>> 0) ^ sc.hOutSumTf) >>> 0);
}

function queryAndFlat(p, sc) {
    const nTerms = 2 + (next32(p) % 2);
    const cc = sc.candCount, cd = sc.candDirty;
    let cdn = 0;
    for (let k = 0; k < nTerms; ++k) {
        const t = pickTermFlat(p);
        const pl = copyPostingsFlat(t, sc);
        if (pl === null) continue;
        const docIds = pl.docIds, n = docIds.length;
        if (k === 0) {
            for (let i = 0; i < n; ++i) {
                const d = docIds[i];
                if (d < N_BASE && cc[d] === 0) { cc[d] = 1; cd[cdn++] = d; }
            }
        } else {
            for (let i = 0; i < n; ++i) {
                const d = docIds[i];
                if (d < N_BASE && cc[d] === k) cc[d] = k + 1;
            }
        }
    }
    let matchSum = 0, matchCount = 0;
    for (let i = 0; i < cdn; ++i) {
        const d = cd[i];
        if (cc[d] === nTerms) { matchSum = (matchSum + mix32(d)) >>> 0; matchCount++; }
        cc[d] = 0;
    }
    return (mix32(matchSum) ^ matchCount) >>> 0;
}

function queryScoredFlat(p, sc, dfSnap) {
    const nTerms = 2 + (next32(p) % 2);
    const cc = sc.candCount, cs = sc.candScore, cd = sc.candDirty;
    let cdn = 0;
    for (let k = 0; k < nTerms; ++k) {
        const t = pickTermFlat(p);
        const df = dfSnap[t];
        const idf = df === 0 ? 0 : ((N_BASE * 1000) / df) | 0;
        const pl = copyPostingsFlat(t, sc);
        if (pl === null) continue;
        const docIds = pl.docIds, tfs = pl.tfs, n = docIds.length;
        for (let i = 0; i < n; ++i) {
            const d = docIds[i];
            if (d < N_BASE) {
                if (cc[d] === 0) { cc[d] = 1; cd[cdn++] = d; cs[d] = 0; }
                cs[d] += tfs[i] * idf;
            }
        }
    }
    // top-TOPN by (score DESC, docId ASC) via partial selection — alloc-free.
    const sD = sc.sortD, sS = sc.sortS;
    for (let i = 0; i < cdn; ++i) { const d = cd[i]; sD[i] = d; sS[i] = cs[d]; cc[d] = 0; }
    const top = cdn < TOPN ? cdn : TOPN;
    for (let k = 0; k < top; ++k) {
        let best = k;
        for (let i = k + 1; i < cdn; ++i) {
            if (sS[i] > sS[best] || (sS[i] === sS[best] && sD[i] < sD[best]))
                best = i;
        }
        if (best !== k) {
            const td = sD[k]; sD[k] = sD[best]; sD[best] = td;
            const ts = sS[k]; sS[k] = sS[best]; sS[best] = ts;
        }
    }
    let h = 0x811c9dc5 >>> 0;
    for (let i = 0; i < top; ++i) h = $imul(h ^ sD[i], 0x01000193) >>> 0;
    for (let i = 0; i < top; ++i) {
        const s = sS[i];
        h = $imul(h ^ (s >>> 0), 0x01000193) >>> 0;
        h = $imul(h ^ ((s / 0x100000000) >>> 0), 0x01000193) >>> 0;
    }
    return h;
}

function phaseBFlat(id) {
    const sc = flatScratch[id];
    const dfSnap = published.dfSnap;
    let csum = 0;
    for (;;) {
        const q = Atomics.add(counters, "nextOp", 1);
        if (q >= N_QUERIES)
            break;
        if (q % WRITER_MOD === 0) {
            // serial-section-tier-pollution: posting lists are Int32Array
            // post-flatten — use the typed-array-aware writer ingest so the
            // phaseA hot path (ingestDocFlat) keeps its plain-[].push profile.
            ingestWriterDocFlat(N_BASE + (q / WRITER_MOD), sc);
            Atomics.add(counters, "writesDone", 1);
            continue;
        }
        const p = sc.prng; p.s = qSeed32(q);
        const kind = next32(p) % 10;
        let h;
        if (kind <= 3)      h = queryPointFlat(p, sc);
        else if (kind <= 7) h = queryAndFlat(p, sc);
        else                h = queryScoredFlat(p, sc, dfSnap);
        csum = (csum + mix32((($imul(q, 0x9e3779b9) >>> 0) ^ h) >>> 0)) >>> 0;
        Atomics.add(counters, "queriesDone", 1);
    }
    partialBFlat[id] = csum;
}

// --- Phase C (flat): 104 groups = (lenBucket-2)*26 + (termId%26). Same
// shard-striped lock fan-in pattern as the spec arm (one Lock per group,
// workers append under it). Per-group storage is two parallel Int32Arrays.
function makeGroupsFlat() {
    const groups = [];
    for (let g = 0; g < 104; ++g)
        groups.push({ lock: new Lock(), totalTf: 0, df: 0, termIds: [], termTfs: [] });
    return groups;
}

function phaseCFlat(id) {
    const groups = published.groups;
    // hold-closure-alloc: see makeFlatScratch. flatScratch[id] was set in
    // phaseAFlat (phase order A→B→C, all workers run A).
    const sc = flatScratch[id];
    const phaseCHold = sc.phaseCHold;
    for (;;) {
        const s = Atomics.add(counters, "nextShard", 1);
        if (s >= K)
            break;
        for (const [t, pl] of flatShards[s]) {
            const tfs = pl.tfs, df = tfs.length;
            let totalTf = 0;
            for (let i = 0; i < df; ++i) totalTf += tfs[i];
            const gk = (termLenBucket(t) - 2) * 26 + (t % 26);
            const g = groups[gk];
            sc.hG = g; sc.hTotalTf = totalTf; sc.hDf = df; sc.hT = t;
            g.lock.hold(phaseCHold);
        }
    }
}

function checksumPhaseCFlat() {
    const groups = published.groups;
    let sum = 0;
    for (let gk = 0; gk < 104; ++gk) {
        const g = groups[gk];
        const ids = g.termIds, tfv = g.termTfs, n = ids.length;
        // top-TOPN by (totalTf DESC, termId ASC) via partial selection.
        const top = n < TOPN ? n : TOPN;
        for (let k = 0; k < top; ++k) {
            let best = k;
            for (let i = k + 1; i < n; ++i) {
                if (tfv[i] > tfv[best] || (tfv[i] === tfv[best] && ids[i] < ids[best]))
                    best = i;
            }
            if (best !== k) {
                let tmp = ids[k]; ids[k] = ids[best]; ids[best] = tmp;
                tmp = tfv[k]; tfv[k] = tfv[best]; tfv[best] = tmp;
            }
        }
        let jh = 0x811c9dc5 >>> 0;
        for (let i = 0; i < top; ++i) {
            jh = $imul(jh ^ ids[i], 0x01000193) >>> 0;
            jh = $imul(jh ^ tfv[i], 0x01000193) >>> 0;
        }
        const item = (mix32(gk) ^ (g.totalTf >>> 0)
            ^ ($imul(g.df, 0x9e3779b9) >>> 0) ^ jh) >>> 0;
        sum = (sum + mix32(item)) >>> 0;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Worker body — all phases, barrier-separated. Thread 0 (the main thread) does
// the single-threaded inter-phase sections and the timing.
// ---------------------------------------------------------------------------
const results = {
    phaseA_ms: 0, phaseB_ms: 0, phaseC_ms: 0, total_ms: 0,
    checksumA: 0n, postings: 0, checksumA2: 0n, checksumB: 0n, checksumC: 0n,
    // S1-serial-ceiling-attribute: per-section wall timers for the thread-0-only
    // inter-phase work (pure measurement; output-only — Go/Java need NOT emit
    // matching keys, fairness-neutral per the task charter). These five sections
    // are the entire gap between (phaseA_ms + phaseB_ms + phaseC_ms) and total_ms.
    postingsChecksum1_ms: 0, buildDfSnap_ms: 0,
    postingsChecksum2_ms: 0, makeGroups_ms: 0, checksumPhaseC_ms: 0,
    // serial-461-decompose (§36): split the two pcN_ms sections into their
    // flatten vs checksum constituents. flatten1_ms is now the wall time of
    // the W-PARALLEL flatten section as seen by thread 0 (its own K/W shards
    // + the barrier tail), reported OUTSIDE postingsChecksum1_ms. cs1_ms ==
    // postingsChecksum1_ms in flat mode. Output-only diagnostic keys; Go/Java
    // need not emit them (fairness-neutral).
    flatten1_ms: 0, cs1_ms: 0, flatten2_ms: 0, cs2_ms: 0,
    // go-gap-liveheap-7x-mark-15pct-postA: wall time of the explicit
    // post-flatten1 fullGC() (FLAT only; thread-0 serial). JS-only diagnostic
    // — Go/Java need not emit it (fairness-neutral; see workerBody comment).
    flattenGC_ms: 0,
};

// §1.11 requires a MONOTONIC clock. The jsc shell's preciseTime() is
// WallTime::now() (CLOCK_REALTIME — NTP can step it mid-run); performance.now()
// is MonotonicTime-based (jsc.cpp functionPerformanceNow) with sub-ms
// resolution, matching Go time.Now/Since and Java System.nanoTime.
function nowMs() {
    return performance.now();
}

function workerBody(id) {
    barrier.await(); // start line
    let t0 = 0, tStart = 0;
    if (id === 0) {
        tStart = nowMs();
        t0 = tStart;
    }

    // Phase A
    if (FLAT) phaseAFlat(id);
    else if (NOMAP) phaseANM(id);
    else if (NOCONCAT) phaseANC();
    else if (INTCS) phaseAI();
    else phaseA();
    barrier.await();
    if (id === 0) {
        results.phaseA_ms = nowMs() - t0;
        t0 = nowMs();
    }
    // flatten1-parallelize-shards-out-of-pc1-timer (§36): W-parallel shard
    // flatten, OUTSIDE the pc1 timer (Go/Java have no flatten — fairness).
    // Each worker owns shards s where s % W === id; no shard Map is touched
    // by two workers. Sources are the SHARED segmented posting lists written
    // under the shard lock in phaseA; the engine-side segwalk-copy makes the
    // Int32Array(segmentedJSArray) copy a fragment-run walk. flatten1_ms is
    // thread 0's wall time for its K/W shards + the barrier tail.
    if (FLAT) flattenFlatShards(id, W);
    barrier.await(); // flatten1 done — all shards normalized to Int32Array
    if (id === 0) {
        if (FLAT) results.flatten1_ms = nowMs() - t0;
        // go-gap-liveheap-7x-mark-15pct-postA: flatten1 just discarded the
        // bulk of the phaseA representation — 130k segmented-butterfly
        // JSArrays holding 4.15M×2 boxed ints — by overwriting the wrapper
        // fields in place with Int32Arrays (the 65k wrappers themselves are
        // now reused, see flatten1-full-FASTER-than-noTA-proves-collision).
        // The OLD set is unreachable but tenured (it was the live posting
        // store for all of phaseA, so every eden cycle promoted it), and JSC
        // sizes the FullCollection trigger off the post-sweep footprint, so
        // phaseB inherits a ~645 MB heap (logGC W=16: Full sweep 729418kb →
        // 645530kb) vs Go's ~86 MB. perf -D 700 W=16 then attributes 15.28%
        // self to GC mark and the run pays ~3 FullCollection cycles at ~29 ms
        // each. Go/Java have NO flatten step (their posting slices/ArrayLists
        // are contiguous from birth — see the flattenFlatShards header), so
        // this discarded representation is a JS-ONLY artefact and collecting
        // it explicitly is the second half of the JS-only flatten cost, not a
        // benchmark cheat: it is honestly accounted INSIDE total_ms. One
        // synchronous fullGC here (workers 1..W-1 are already racing toward /
        // parked at the dfSnap barrier — pure STW, no mutator interleaving)
        // drops the live set to the ~55 MB post-flatten Int32Array state, so
        // every subsequent phaseB mark cycle is ~10× cheaper and the
        // FullCollection trigger re-bases off the small heap. Net: pay ~1
        // cycle up front, save ~3 inflated cycles + 15% helper-thread mark
        // CPU during phaseB. estMs ≈ 45.
        if (FLAT) {
            let g0 = nowMs();
            fullGC();
            results.flattenGC_ms = nowMs() - g0;
        }
        // w1-cs1-cold-vs-warm-45ms (§37 MEASURED-AND-REJECTED): a small-prefix
        // pre-warm `postingsChecksumFlat(8)` here was proposed to absorb the
        // two Overflow recompiles + FTL install outside the cs1 timer. Direct
        // A/B at W=16: WITHOUT prewarm cs1 {67,68,79,82} stable; WITH prewarm
        // cs1 bimodal {44-77} / {122-241} — the 8-shard call's return path
        // executes once, the second DFG compile (post-Overflow-reopt) sees an
        // under-profiled tail, and the FTL/FTLForOSREntry install races the
        // prewarm→cs1 call boundary, so ~1/3 of reps land in a recompile slow
        // band. Net median ~+10ms and ~7× variance. The `sEnd` parameter is
        // kept (both real callers pass K) so a future per-function tier-up
        // hint can revisit this without a signature change.
        let s0 = nowMs();
        if (FLAT) {
            const r = postingsChecksumFlat(K);
            results.cs1_ms = nowMs() - s0;
            results.checksumA = BigInt(r.sum); results.postings = r.postings;
        } else if (NOMAP) {
            const r = postingsChecksumNM();
            results.checksumA = r.sum; results.postings = r.postings;
        } else {
            const { sum, postings } = (INTCS || NOCONCAT) ? postingsChecksumInt() : postingsChecksum();
            results.checksumA = sum; results.postings = postings;
        }
        results.postingsChecksum1_ms = nowMs() - s0;
        s0 = nowMs();
        published.dfSnap = FLAT ? buildDfSnapFlat()
            : NOMAP ? buildDfSnapNM()
            : buildDfSnap(); // frozen at the A/B barrier (§1.7)
        results.buildDfSnap_ms = nowMs() - s0;
    }
    barrier.await(); // dfSnap published

    // Phase B
    if (id === 0)
        t0 = nowMs();
    if (FLAT) phaseBFlat(id);
    else if (NOMAP) phaseBNM(id);
    else if (NOCONCAT) phaseBNC(id);
    else if (INTCS) phaseBI(id);
    else phaseB(id);
    barrier.await();
    if (id === 0) {
        results.phaseB_ms = nowMs() - t0;
        if (FLAT) {
            let cs = 0;
            for (let i = 0; i < W; ++i) cs = (cs + partialBFlat[i]) >>> 0;
            results.checksumB = BigInt(cs);
        } else if (INTCS || NOCONCAT || NOMAP) {
            let cs = 0;
            for (let i = 0; i < W; ++i) cs = (cs + partialBI[i]) >>> 0;
            results.checksumB = BigInt(cs);
        } else {
            let cs = 0n;
            for (let i = 0; i < W; ++i) cs = (cs + partialB[i]) & MASK;
            results.checksumB = cs;
        }
        t0 = nowMs();
    }
    // flatten2-serial-thread0-not-striped-java-pays-ze (§37 R4-(3)): the
    // post-phaseB re-normalize was running SERIAL on thread 0 as
    // `flattenFlatShards(0, 1)` — 62-66 ms W-flat — while flatten1 (above) is
    // already W-PARALLEL shard-striped. The work is identical in shape:
    // phaseB writers (a) geometric-grew some lists in place from foreign
    // threads (ingestWriterHold: `.len` < cap, pl wrapper SW=1), (b) created
    // fresh pl objects on worker threads for brand-new terms; flatten2 trims
    // each pl to an exact-sized Int32Array so pc2 sees the same monomorphic
    // shape pc1 did. Sources are already Int32Array → same-type memcpy. Each
    // shard's pl wrappers are touched only by the stripe worker that owns
    // s % W === id (same independence flatten1 relies on), so this is the
    // same call with (id, W) between barriers. Java/Go have NO flatten step,
    // so like flatten1 this is reported OUTSIDE postingsChecksum2_ms
    // (cs2_ms == postingsChecksum2_ms in flat mode). 63 ms → ~63/W + barrier
    // at W≤8 (W=8 62.6→21.2); W>8 the parallel Int32Array.slice fan-out hits
    // the SAME Gigacage/ArrayBufferContents allocator knee filed as residual
    // R4 (W=16 measured 62.26 ≈ serial), so this is wall-neutral there but is
    // the prerequisite for an R4 fix to benefit flatten2 as well as flatten1.
    if (FLAT) flattenFlatShards(id, W);
    barrier.await(); // flatten2 done — all shards trimmed to exact Int32Array
    if (id === 0) {
        if (FLAT) results.flatten2_ms = nowMs() - t0;
        let s0 = nowMs();
        if (FLAT) {
            results.checksumA2 = BigInt(postingsChecksumFlat(K).sum);
            results.cs2_ms = nowMs() - s0;
        } else if (NOMAP)
            results.checksumA2 = postingsChecksumNM().sum;
        else
            results.checksumA2 = ((INTCS || NOCONCAT) ? postingsChecksumInt() : postingsChecksum()).sum;
        results.postingsChecksum2_ms = nowMs() - s0;
        s0 = nowMs();
        if (FLAT) {
            published.groups = makeGroupsFlat();
        } else if (NOMAP) {
            published.groups = makeGroupsNM();
        } else {
            const { groups, keys } = makeGroups(); // pre-populated before Phase C (§1.10)
            published.groups = groups;
            published.groupKeys = keys;
        }
        results.makeGroups_ms = nowMs() - s0;
    }
    barrier.await(); // groups published

    // Phase C
    if (id === 0)
        t0 = nowMs();
    if (FLAT)
        phaseCFlat(id);
    else if (NOMAP)
        phaseCNM();
    else if (WS_MODE)
        phaseCWS(id);
    else
        phaseC();
    barrier.await();
    if (id === 0) {
        results.phaseC_ms = nowMs() - t0;
        if (WS_MODE && !FLAT && !NOMAP)
            wsMergeLocals(); // single-threaded merge of thread-local accumulators
        let s0 = nowMs();
        if (FLAT) {
            flattenGroupsFlat(); // serial-seg-getbutterfly-osrexit (group arrays)
            results.checksumC = BigInt(checksumPhaseCFlat());
        } else if (NOMAP)
            results.checksumC = BigInt(checksumPhaseCNM());
        else if (INTCS || NOCONCAT)
            results.checksumC = BigInt(checksumPhaseCI());
        else
            results.checksumC = checksumPhaseC();
        results.checksumPhaseC_ms = nowMs() - s0;
        results.total_ms = nowMs() - tStart;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
const threads = [];
for (let id = 1; id < W; ++id)
    threads.push(new Thread(workerBody, id));
workerBody(0);
for (const t of threads)
    t.join();

function hex16(x) {
    return (x & MASK).toString(16).padStart(16, "0");
}

function ms(x) {
    return Math.round(x * 1000) / 1000;
}

const armTag = NOMAP ? ',"arm":"nomap"' : NOCONCAT ? ',"arm":"noconcat"' : INTCS ? ',"intcs":true' : '';
print('{"impl":"js"' + armTag + (FLAT ? ',"flat":true' : '') + ',"threads":' + W
    + (WS_MODE ? ',"mode":"ws"' : '')
    + ',"phaseA_ms":' + ms(results.phaseA_ms)
    + ',"phaseB_ms":' + ms(results.phaseB_ms)
    + ',"phaseC_ms":' + ms(results.phaseC_ms)
    + ',"total_ms":' + ms(results.total_ms)
    + ',"checksumA":"' + hex16(results.checksumA) + '"'
    + ',"postings":' + results.postings
    + ',"checksumA2":"' + hex16(results.checksumA2) + '"'
    + ',"checksumB":"' + hex16(results.checksumB) + '"'
    + ',"checksumC":"' + hex16(results.checksumC) + '"'
    + ',"docsIngested":' + counters.docsIngested
    + ',"tokensProcessed":' + counters.tokensProcessed
    + ',"writesDone":' + counters.writesDone
    // S1-serial-ceiling-attribute: thread-0-only inter-phase section walls
    // (JS-only diagnostic keys; fairness-neutral — Go/Java need not emit them).
    + ',"postingsChecksum1_ms":' + ms(results.postingsChecksum1_ms)
    + ',"buildDfSnap_ms":' + ms(results.buildDfSnap_ms)
    + ',"postingsChecksum2_ms":' + ms(results.postingsChecksum2_ms)
    + ',"makeGroups_ms":' + ms(results.makeGroups_ms)
    + ',"checksumPhaseC_ms":' + ms(results.checksumPhaseC_ms)
    // serial-461-decompose (§36): flatten-vs-checksum split (FLAT only;
    // emit 0 in non-flat mode). JS-only diagnostic; fairness-neutral.
    + ',"flatten1_ms":' + ms(results.flatten1_ms)
    + ',"cs1_ms":' + ms(results.cs1_ms)
    + ',"flatten2_ms":' + ms(results.flatten2_ms)
    + ',"cs2_ms":' + ms(results.cs2_ms)
    + ',"flattenGC_ms":' + ms(results.flattenGC_ms)
    + '}');
