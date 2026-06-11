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
        { let lc = ""; for (let k = 0; k < codes.length; ++k) lc += String.fromCharCode(codes[k]); out.push(lc); } // NOSPREAD variant
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
// Worker body — all phases, barrier-separated. Thread 0 (the main thread) does
// the single-threaded inter-phase sections and the timing.
// ---------------------------------------------------------------------------
const results = {
    phaseA_ms: 0, phaseB_ms: 0, phaseC_ms: 0, total_ms: 0,
    checksumA: 0n, postings: 0, checksumA2: 0n, checksumB: 0n, checksumC: 0n,
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
    phaseA();
    barrier.await();
    if (id === 0) {
        results.phaseA_ms = nowMs() - t0;
        const { sum, postings } = postingsChecksum();
        results.checksumA = sum;
        results.postings = postings;
        published.dfSnap = buildDfSnap(); // frozen at the A/B barrier (§1.7)
    }
    barrier.await(); // dfSnap published

    // Phase B
    if (id === 0)
        t0 = nowMs();
    phaseB(id);
    barrier.await();
    if (id === 0) {
        results.phaseB_ms = nowMs() - t0;
        let cs = 0n;
        for (let i = 0; i < W; ++i)
            cs = (cs + partialB[i]) & MASK;
        results.checksumB = cs;
        results.checksumA2 = postingsChecksum().sum;
        const { groups, keys } = makeGroups(); // pre-populated before Phase C (§1.10)
        published.groups = groups;
        published.groupKeys = keys;
    }
    barrier.await(); // groups published

    // Phase C
    if (id === 0)
        t0 = nowMs();
    phaseC();
    barrier.await();
    if (id === 0) {
        results.phaseC_ms = nowMs() - t0;
        results.checksumC = checksumPhaseC(); // thread 0 sorts + topN after barrier
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

print('{"impl":"js","threads":' + W
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
    + '}');
