// TD-R2-2 experiment part 1: ZERO-TEARDOWN variant of
// Tools/threads/scalebench/js/repro-bigint-shared-ingest.js.
// Workers NEVER exit before verification: after finishing their docs they
// increment counters.workersDone and park in property Atomics.wait on
// park.go. Main runs the FULL verification walk while all 3 spawned lites
// are Live (no ~GCClient::Heap / tearDownSpawnedThreadForExit /
// stopAllocatingForGood has run). Only AFTER printing OK/CORRUPT does main
// release the parked workers and join them.
// If corruption still reproduces here at a rate comparable to the original,
// every teardown-vs-GC window is bounded OUT of the primary bug.

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
const A_CODE = 97;
function termOf(i) { let s = ""; do { s = String.fromCharCode(A_CODE + (i % 26)) + s; i = (i / 26) | 0; } while (i > 0); return "t" + s; }
function pickTerm(p) { const a = Number(next(p) % Vn), b = Number(next(p) % Vn); return a < b ? a : b; }
function genDocText(d) {
    const p = { s: mix(GLOBAL_SEED ^ ((BigInt(d) * GOLDEN) & MASK)) };
    const titleLen = 5 + Number(next(p) % 8n), bodyLen = 80 + Number(next(p) % 121n);
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
const counters = { nextDoc: 0, docsIngested: 0, tokensProcessed: 0, workersDone: 0 };
const park = { go: 0 };
const sleepLane = { v: 0 };
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
function workerMain() {
    work();
    Atomics.add(counters, "workersDone", 1);
    // Park WITHOUT exiting: no teardown runs until main releases us.
    while (Atomics.load(park, "go") === 0)
        Atomics.wait(park, "go", 0);
    return 0;
}
const threads = []; for (let i = 1; i < 4; i++) threads.push(new Thread(workerMain));
work();
// Wait for all workers to finish ingest and park (bounded property-wait
// sleep steps; the property park drops the GIL — harmless GIL-off).
while (Atomics.load(counters, "workersDone") !== 3)
    Atomics.wait(sleepLane, "v", 0, 5);
if (counters.docsIngested !== N_BASE) throw new Error("docsIngested=" + counters.docsIngested);
// FULL verification while all 3 worker lites are LIVE (parked, not torn down).
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
// Verification done with zero teardowns. NOW release and join.
Atomics.store(park, "go", 1);
Atomics.notify(park, "go");
for (const t of threads) t.join();
print("JOINED");
