// Minimization candidate for the shared-heap under-marking corruption.
// Params via -e style globals or defaults:
//   THREADS (total incl. main, default 4)
//   ITERS   (per-thread outer iterations, default 200000)
//   CHURN   ("bigint" | "object" | "string" | "double" | "none")
//   SHARED  ("array" | "map" | "obj")
//   LOCKED  (1 = use Lock around shared append, 0 = racy)
// Each thread: churn allocation + occasionally append {marker} to shared
// structure; at the end verify every element of shared arrays.
const T = (typeof THREADS !== "undefined") ? THREADS : 4;
const ITERS = (typeof NITERS !== "undefined") ? NITERS : 200000;
const CHURN = (typeof CHURNK !== "undefined") ? CHURNK : "bigint";
const SHARED = (typeof SHAREDK !== "undefined") ? SHAREDK : "array";
const MASK = 0xFFFFFFFFFFFFFFFFn;
const GOLDEN = 0x9E3779B97F4A7C15n;

const NSHARDS = 16;
const shards = [];
for (let i = 0; i < NSHARDS; i++) shards.push({ lock: new Lock(), arr: [], map: new Map(), n: 0 });
const counters = { done: 0 };

function makeChurn(kind, seedN) {
    if (kind === "bigint") {
        let s = BigInt(seedN) * GOLDEN & MASK;
        return function () {
            // splitmix64 step: allocates several heap BigInts per call
            s = (s + GOLDEN) & MASK;
            let z = s;
            z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & MASK;
            z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & MASK;
            return Number((z ^ (z >> 31n)) & 0xFFFFn);
        };
    }
    if (kind === "object") {
        let s = seedN >>> 0;
        return function () {
            s = (s * 1103515245 + 12345) >>> 0;
            const o = { a: s, b: s ^ 0xdead, c: { d: s } };
            return (o.a + o.c.d) & 0xFFFF;
        };
    }
    if (kind === "string") {
        let s = seedN >>> 0;
        return function () {
            s = (s * 1103515245 + 12345) >>> 0;
            const str = "x" + s + "y" + (s ^ 0xbeef);
            return str.length & 0xFFFF;
        };
    }
    if (kind === "double") {
        let s = seedN >>> 0;
        return function () {
            s = (s * 1103515245 + 12345) >>> 0;
            const a = [s * 1.5, s * 2.5, s * 3.5];
            return a[s % 3] & 0xFFFF;
        };
    }
    let s = seedN >>> 0;
    return function () { s = (s * 1103515245 + 12345) >>> 0; return s & 0xFFFF; };
}

function work(tid) {
    const churn = makeChurn(CHURN, tid + 1);
    for (let i = 0; i < ITERS; i++) {
        const h = churn();
        if ((i & 15) === 0) { // shared append every 16 iters
            const shard = shards[h % NSHARDS];
            const val = tid * 16777216 + (i & 0xFFFFFF); // encode writer+iter
            shard.lock.hold(() => {
                if (SHARED === "array") {
                    shard.arr.push(val);
                    shard.arr.push(val + 1);
                } else if (SHARED === "map") {
                    let p = shard.map.get(h);
                    if (p === undefined) { p = { docIds: [], tfs: [] }; shard.map.set(h, p); }
                    p.docIds.push(val);
                    p.tfs.push(val + 1);
                } else {
                    shard["k" + (h & 63)] = val;
                }
                shard.n++;
            });
        }
    }
    Atomics.add(counters, "done", 1);
}

const threads = [];
for (let t = 1; t < T; t++) { const tid = t; threads.push(new Thread(() => work(tid))); }
work(0);
for (const th of threads) th.join();

let bad = 0, checked = 0;
function checkPair(a, b, where) {
    if (a === undefined && b === undefined) return;
    if (typeof a !== "number" || typeof b !== "number" || b !== a + 1) {
        print("BAD " + where + ": a=" + typeof a + ":" + String(a).substring(0, 40) + " b=" + typeof b + ":" + String(b).substring(0, 40));
        bad++;
    }
}
for (let si = 0; si < NSHARDS; si++) {
    const s = shards[si];
    if (SHARED === "array") {
        if (s.arr.length % 2) { print("BAD ODD LEN shard" + si + " len=" + s.arr.length); bad++; }
        for (let i = 0; i + 1 < s.arr.length; i += 2) { checkPair(s.arr[i], s.arr[i + 1], "shard" + si + "[" + i + "]"); checked++; if (bad > 10) break; }
    } else if (SHARED === "map") {
        for (const [k, p] of s.map) {
            if (!p || !p.docIds || p.docIds.length !== p.tfs.length) { print("BAD LIST shard" + si + " key=" + String(k).substring(0, 30)); bad++; continue; }
            for (let i = 0; i < p.docIds.length; i++) { checkPair(p.docIds[i], p.tfs[i], "shard" + si + "/" + k + "[" + i + "]"); checked++; }
            if (bad > 10) break;
        }
    }
    if (bad > 10) break;
}
print(bad === 0 ? ("OK checked=" + checked) : ("CORRUPT bad=" + bad));
