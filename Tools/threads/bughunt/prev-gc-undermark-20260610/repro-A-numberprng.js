// ENGINE-BUG REPRO (blocks SCALEBENCH at W >= 4 on this branch) — found while
// implementing Tools/threads/scalebench/js/bench.js (2026-06-10, Release jsc
// mtime Jun 10 03:06).
//
// STATUS 2026-06-10 LATER (scalebench run phase; same Release jsc 03:34,
// ninja-verified current with the tree): narrowed substantially —
//   * GC-DEPENDENT: --useGC=0 -> OK 4/4 (zero collections); GC on -> CORRUPT
//     ~100%. --forceRAMSize up to 64G does not suppress the eden cycles.
//   * NOT marking-parallelism: --numberOfGCMarkers=1 still corrupts 4/4.
//   * NOT generational/remembered-set: --useGenerationalGC=0 (full GCs only)
//     still corrupts 4/4. --useConcurrentGC=0 still corrupts.
//   * --sweepSynchronously=1 turns the silent aliasing into immediate
//     crashes/exceptions => LIVE cells are being swept (under-marking during
//     the STW collection), and lazy sweep normally delays the re-allocation.
//   * --useSharedGCHeap=0 (other 5 flags kept) -> OK 3/3;
//     --useSharedAtomStringTable=0 (sharedGCHeap kept) -> still corrupts.
//   * Corruption shape (diagnostic variant): a posting list's docIds
//     butterfly ALIASES another thread's local tf-Map storage — term
//     strings and 1-counts interleaved with real docIds; lengths diverge.
//     Consistent with: cell allocated by thread T in-flight at the GC
//     handshake (held only in T's registers/stack) is missed by the
//     conservative scan / newlyAllocated accounting, swept, and handed to
//     another thread's allocator.
//   * Release-only in practice: Debug (03:38, same tree) OK 2/2; TSan jsc
//     (rebuilt from this tree) OK with 0 reports — timing/instrumentation
//     mask, so TSAN will not name this one.
//   * GIL-on (--useJSThreads=1 only) OK 2/2.
// STATUS 2026-06-10 (Release jsc mtime Jun 10 03:34): still reproduces 3/3
// (CORRUPT bad=4..10). bench.js at W=4 smoke now segfaults outright (libstdc++
// std::array<unsigned long, 16> operator[] assertion, core dump) in addition
// to the earlier BigInt-parse corruption mode. This is a GATING engine bug:
// no W>=4 JS cell can produce a valid result, so the whole matrix is blocked
// until it is fixed. run.sh preflight smoke now includes a W=4 leg for all
// three languages so any recurrence aborts in preflight, not mid-matrix.
//
// Symptom: with the pinned GIL-off flag set
//   --useJSThreads=1 --useThreadGIL=0 --useVMLite=1
//   --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1
// 4 threads doing (a) heavy thread-local BigInt arithmetic (the spec-mandated
// splitmix64 PRNG) interleaved with (b) Lock-protected appends to shared
// posting-list arrays/Maps corrupt the shared heap: parallel arrays appended
// under ONE Lock.hold diverge in length (e.g. docIds=2 vs tfs=6; once
// docIds=465 vs tfs=9), elements read back as strings/undefined, lengths read
// back as garbage (3849972257), and some runs segfault outright.
// Reproduces ~3/3 at W=4 with N=2000 docs.
//
// Isolation matrix (3 runs each):
//   BigInt PRNG + shared locked ingest            -> CORRUPT 3/3  (this file)
//   Number PRNG + same shared locked ingest       -> OK 3/3
//   BigInt PRNG churn, thread-local only          -> OK 3/3
//   BigInt PRNG + string churn, thread-local only -> OK 3/3
//   BigInt fnv1a + Number PRNG + shared ingest    -> OK 3/3
// => trigger is BigInt allocation churn CONCURRENT WITH cross-thread shared
//    object/array writes (likely the JSBigInt allocation/GC path under
//    useSharedGCHeap), not BigInt arithmetic itself and not the locked
//    data structure pattern itself.
//
// Run:
//   WebKitBuild/Release/bin/jsc <flags above> js/repro-bigint-shared-ingest.js
// Expected output when fixed: "OK postings=144814" (deterministic).
// Current output: "CORRUPT bad=N" lines naming posting lists whose parallel
// arrays diverged, or a segfault.
//
// SPEC.md §1.1 REQUIRES BigInt for the PRNG/checksum paths in JS, so
// bench.js cannot avoid this; until fixed, the full matrix (W in {4..32})
// will fail the §5.5 checksum gate or crash. W=1 and W=2 have been observed
// clean (calibration and smoke are unaffected).

const MASK = 0xFFFFFFFFFFFFFFFFn;
const GLOBAL_SEED = 0x5CA1AB1E0BADF00Dn;
const GOLDEN = 0x9E3779B97F4A7C15n;
const Vn = 65536n, K = 128, Kn = 128n, N_BASE = 2000;
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
const FNV_OFFSET = 0xCBF29CE484222325n, FNV_PRIME = 0x100000001B3n;
const BYTE = []; for (let i = 0; i < 256; i++) BYTE.push(BigInt(i));
function fnv1a(s) { let h = FNV_OFFSET; for (let i = 0; i < s.length; i++) h = ((h ^ BYTE[s.charCodeAt(i)]) * FNV_PRIME) & MASK; return h; }
const A_CODE = 97;
function termOf(i) { let s = ""; do { s = String.fromCharCode(A_CODE + (i % 26)) + s; i = (i / 26) | 0; } while (i > 0); return "t" + s; }
function nnext(p){ p.t = (p.t * 1103515245 + 12345) >>> 0; return p.t; }
function pickTerm(p) { if (p.t===undefined) p.t = Number(p.s & 0xFFFFFFn); const a = nnext(p) % 65536, b = nnext(p) % 65536; return a < b ? a : b; }
function genDocText(d) {
    const p = { s: mix(GLOBAL_SEED ^ ((BigInt(d) * GOLDEN) & MASK)) };
    p.t = Number(p.s & 0xFFFFFFn);
    const titleLen = 5 + nnext(p) % 8, bodyLen = 80 + nnext(p) % 121;
    const n = titleLen + bodyLen, parts = [];
    for (let j = 0; j < n; j++) {
        let tok = termOf(pickTerm(p));
        if (j % 7 === 0) tok = tok.charAt(0).toUpperCase() + tok.substring(1);
        if (j % 11 === 3) tok += ",";
        if (j % 13 === 12) tok += ".";
        parts.push(tok);
    }
    return parts.join(" ");
}
const SPLIT_RE = /[^a-zA-Z0-9]+/;
function tokenize(text) {
    const pieces = text.split(SPLIT_RE), out = [];
    for (let i = 0; i < pieces.length; i++) { const x = pieces[i]; if (x.length) out.push(x.toLowerCase()); }
    return out;
}
const shards = []; for (let i = 0; i < K; i++) shards.push({ lock: new Lock(), map: new Map() });
const counters = { nextDoc: 0, docsIngested: 0, tokensProcessed: 0 };
function ingestDoc(d) {
    const toks = tokenize(genDocText(d));
    const tf = new Map();
    for (let i = 0; i < toks.length; i++) { const t = toks[i]; const c = tf.get(t); tf.set(t, c === undefined ? 1 : c + 1); }
    for (const [term, count] of tf) {
        const shard = shards[(function(){let h=0;for(let i=0;i<term.length;i++)h=(h*31+term.charCodeAt(i))>>>0;return h%K;})()];
        shard.lock.hold(() => {
            let p = shard.map.get(term);
            if (p === undefined) { p = { docIds: [], tfs: [] }; shard.map.set(term, p); }
            p.docIds.push(d);
            p.tfs.push(count);
        });
    }
    Atomics.add(counters, "tokensProcessed", toks.length);
    Atomics.add(counters, "docsIngested", 1);
}
function work() { for (;;) { const d = Atomics.add(counters, "nextDoc", 1); if (d >= N_BASE) break; ingestDoc(d); } }
const threads = []; for (let i = 1; i < 4; i++) threads.push(new Thread(work));
work();
for (const t of threads) t.join();
if (counters.docsIngested !== N_BASE) throw new Error("docsIngested=" + counters.docsIngested);
let postings = 0, bad = 0;
for (const s of shards) for (const [term, p] of s.map) {
    if (typeof term !== "string") { print("BAD KEY type=" + typeof term); bad++; continue; }
    if (!p || p.docIds.length !== p.tfs.length) { print("BAD LIST " + term); bad++; continue; }
    for (let i = 0; i < p.docIds.length; i++) {
        if (typeof p.docIds[i] !== "number" || typeof p.tfs[i] !== "number") {
            print("BAD ELEM " + term + "[" + i + "/" + p.docIds.length + "]: " + typeof p.docIds[i] + "=" + String(p.docIds[i]).substring(0, 30) + " / " + typeof p.tfs[i]);
            if (++bad > 10) break;
        }
        postings++;
    }
}
print(bad === 0 ? ("OK postings=" + postings) : ("CORRUPT bad=" + bad));
