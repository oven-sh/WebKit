// Minimized from repro-bigint-shared-ingest.js: no strings, no regex, no
// per-doc Map, no fnv. Kept: BigInt splitmix64 churn (amplifier), shared
// Map-of-{docIds,tfs} posting lists appended under Lock, work-stealing via
// Atomics counter. Term keys are plain integers.
const MASK = 0xFFFFFFFFFFFFFFFFn;
const GOLDEN = 0x9E3779B97F4A7C15n;
function next(p) {
    p.s = (p.s + GOLDEN) & MASK;
    let z = p.s;
    z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & MASK;
    z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & MASK;
    return z ^ (z >> 31n);
}
const K = 128, N_BASE = 2000;
const shards = []; for (let i = 0; i < K; i++) shards.push({ lock: new Lock(), map: new Map() });
const counters = { nextDoc: 0, docsIngested: 0 };
function ingestDoc(d) {
    const p0 = { s: (BigInt(d) * GOLDEN) & MASK };
    const nTok = 85 + Number(next(p0) % 129n);
    const CH = (typeof CHURNX !== "undefined") ? CHURNX : 8;
    for (let j = 0; j < nTok; j++) {
        for (let c = 0; c < CH; c++) next(p0); // extra BigInt allocation churn
        const a = Number(next(p0) % 65536n), b = Number(next(p0) % 65536n);
        const term = a < b ? a : b;
        const shard = shards[term % K];
        shard.lock.hold(() => {
            let p = shard.map.get(term);
            if (p === undefined) { p = { docIds: [], tfs: [] }; shard.map.set(term, p); }
            p.docIds.push(d);
            p.tfs.push(d + 1);
        });
    }
    Atomics.add(counters, "docsIngested", 1);
}
function work() { for (;;) { const d = Atomics.add(counters, "nextDoc", 1); if (d >= N_BASE) break; ingestDoc(d); } }
const NT = (typeof NTHREADS !== "undefined") ? NTHREADS : 4;
const threads = []; for (let i = 1; i < NT; i++) threads.push(new Thread(work));
work();
for (const t of threads) t.join();
if (counters.docsIngested !== N_BASE) throw new Error("docsIngested=" + counters.docsIngested);
let postings = 0, bad = 0;
for (const s of shards) for (const [term, p] of s.map) {
    if (typeof term !== "number") { print("BAD KEY type=" + typeof term + " " + String(term).substring(0, 40)); bad++; continue; }
    if (!p || !p.docIds || p.docIds.length !== p.tfs.length) { print("BAD LIST term=" + term + " docIds.len=" + (p && p.docIds && p.docIds.length) + " tfs.len=" + (p && p.tfs && p.tfs.length)); bad++; continue; }
    for (let i = 0; i < p.docIds.length; i++) {
        const a = p.docIds[i], b = p.tfs[i];
        if (typeof a !== "number" || typeof b !== "number" || b !== a + 1) {
            print("BAD ELEM term=" + term + "[" + i + "/" + p.docIds.length + "]: " + typeof a + "=" + String(a).substring(0, 40) + " / " + typeof b + "=" + String(b).substring(0, 40));
            if (++bad > 10) break;
        }
        postings++;
    }
    if (bad > 10) break;
}
print(bad === 0 ? ("OK postings=" + postings) : ("CORRUPT bad=" + bad));
