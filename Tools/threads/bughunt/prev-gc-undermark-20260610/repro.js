// repro.js — instrumented copy of Tools/threads/scalebench/js/repro-bigint-shared-ingest.js
// (shared-GC-heap under-marking bughunt, 2026-06-10). Workload identical to the
// original; only the END-OF-RUN VERIFIER is enriched:
//   - decodes every mismatch (docIds[i] should be a doc number 0..1999;
//     tfs[i] a small count 1..~20)
//   - dumps $vm.value() for the posting object and both arrays (butterfly ptr,
//     structure, indexing type)
//   - prints the full contents of corrupted lists (typeof + value, first 48)
//   - GLOBAL BUTTERFLY ALIASING SCAN: collects the butterfly pointer of every
//     posting array via $vm.value() and reports any two DISTINCT arrays that
//     share a butterfly base (the smoking gun for swept-and-reallocated).
// Run with --useDollarVM=1 added to the pinned GIL-off flags.
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

// ---------- instrumented verifier ----------
const haveVM = (typeof $vm === "object");
function vmDesc(x) { try { return haveVM ? $vm.value(x) : "(no $vm)"; } catch (e) { return "(vm err " + e + ")"; } }
function bflyOf(desc) { const m = /butterfly 0x([0-9a-f]+)\(base=0x([0-9a-f]+)\)/.exec(desc); return m ? m[2] : null; }
function idxMode(x) { try { return haveVM ? $vm.indexingMode(x) : "?"; } catch (e) { return "(err)"; } }
function dumpList(name, arr) {
    let s = name + " len=" + arr.length + " mode=" + idxMode(arr) + "\n  " + vmDesc(arr) + "\n  [";
    const lim = Math.min(arr.length, 48);
    for (let i = 0; i < lim; i++) {
        const v = arr[i];
        s += (i ? ", " : "") + (typeof v === "number" ? v : typeof v + ":" + String(v).substring(0, 24));
    }
    if (arr.length > lim) s += ", ...";
    print(s + "]");
}
let postings = 0, bad = 0;
const allLists = []; // {label, arr} for the aliasing scan
for (let si = 0; si < K; si++) {
    const s = shards[si];
    for (const [term, p] of s.map) {
        if (typeof term !== "string") { print("BAD KEY shard=" + si + " type=" + typeof term + " val=" + String(term).substring(0, 40) + " desc=" + vmDesc(term)); bad++; continue; }
        if (!p || typeof p !== "object") { print("BAD POSTING shard=" + si + " term=" + term + " typeof=" + typeof p); bad++; continue; }
        allLists.push({ label: term + ".docIds", arr: p.docIds });
        allLists.push({ label: term + ".tfs", arr: p.tfs });
        if (p.docIds.length !== p.tfs.length) {
            bad++;
            print("== BAD LIST shard=" + si + " term=" + JSON.stringify(term) + " docIds.len=" + p.docIds.length + " tfs.len=" + p.tfs.length);
            print("  posting obj: " + vmDesc(p) + " keys=" + Object.keys(p).join(","));
            dumpList("  docIds", p.docIds);
            dumpList("  tfs", p.tfs);
            continue;
        }
        for (let i = 0; i < p.docIds.length; i++) {
            const a = p.docIds[i], b = p.tfs[i];
            const aOk = typeof a === "number" && Number.isInteger(a) && a >= 0 && a < N_BASE;
            const bOk = typeof b === "number" && Number.isInteger(b) && b >= 1 && b <= 250;
            if (!aOk || !bOk) {
                bad++;
                print("== BAD ELEM shard=" + si + " term=" + JSON.stringify(term) + " i=" + i + "/" + p.docIds.length
                    + " docIds[i]=" + typeof a + ":" + String(a).substring(0, 40)
                    + " tfs[i]=" + typeof b + ":" + String(b).substring(0, 40));
                print("  posting obj: " + vmDesc(p));
                dumpList("  docIds", p.docIds);
                dumpList("  tfs", p.tfs);
                break; // one detailed dump per list
            }
            postings++;
        }
        if (bad > 40) break;
    }
    if (bad > 40) break;
}
// aliasing scan: do any two distinct arrays share a butterfly base?
if (haveVM) {
    const seen = new Map(); let aliases = 0;
    for (const { label, arr } of allLists) {
        if (arr.length === 0) continue; // empty arrays may legitimately share null/sentinel
        const b = bflyOf(vmDesc(arr));
        if (!b) continue;
        const prev = seen.get(b);
        if (prev !== undefined && prev.arr !== arr) {
            aliases++;
            print("!! BUTTERFLY ALIAS base=0x" + b + " : " + prev.label + " (len=" + prev.arr.length + ") AND " + label + " (len=" + arr.length + ")");
            if (aliases > 20) break;
        } else seen.set(b, { label, arr });
    }
    print("aliasScan: lists=" + allLists.length + " aliases=" + aliases);
}
print(bad === 0 ? ("OK postings=" + postings) : ("CORRUPT bad=" + bad));
