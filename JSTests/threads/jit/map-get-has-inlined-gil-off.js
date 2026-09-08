//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-jit history §35 (seventh landing round). GIL off, Map.prototype.get/has
// and Set.prototype.has are inlined by the DFG/FTL again; the FTL's probe
// validates against the table's seqlock version and falls back to the
// runtime's validated reader when a writer overlapped it. (1) Torn-read
// freedom: FTL-hot readers race a writer that inserts, overwrites, deletes,
// clears and forces rehashes; every get() must return a value the key could
// have held (its own index, as a number or boxed in an object) or undefined -
// never the deleted sentinel, a chain index, or another key's value - and
// has()/Set.has() must return booleans. (2) The inline path is real: a
// single-threaded FTL get loop no longer reaches the runtime reader once per
// call (before: 300000 of 300000; after: the DFG-tier stragglers only, 0 in a
// quiet run).
load("../harness.js", "caller relative");

const N = 2048;
const map = new Map(), set = new Set();
for (let i = 0; i < N; ++i) { map.set(i, i); map.set("k" + i, { i }); set.add(i); }
const box = { stop: 0 };

function readOnce(k) {
    const v = map.get(k);
    if (v !== undefined && v !== k) return "key " + k + " read " + String(v);
    const s = map.get("k" + (k & 255));
    if (s !== undefined && (typeof s !== "object" || s === null || s.i !== (k & 255))) return "key k" + (k & 255) + " read " + String(s);
    const h = map.has(k), sh = set.has(k);
    if (typeof h !== "boolean" || typeof sh !== "boolean") return "has returned " + typeof h + "/" + typeof sh;
    return null;
}
noInline(readOnce);
function reader() {
    let bad = null, n = 0;
    // Bounded as well as stop-driven: GIL on, a spinning thread is never
    // preempted, so an unbounded reader scheduled before the writer would hang.
    for (let round = 0; round < 400 && !Atomics.load(box, "stop") && !bad; ++round) {
        for (let k = 0; k < N && !bad; ++k, ++n) bad = readOnce(k);
    }
    return bad || ("ok " + n);
}
const GENERATIONS = 60;
const writer = new Thread(() => {
    let gen = 0;
    for (; gen < GENERATIONS; ++gen) {
        for (let i = 0; i < N; i += 3) { map.delete(i); set.delete(i); }       // deletes (and shrink rehash)
        for (let i = 0; i < N; i += 3) { map.set(i, i); set.add(i); }           // re-inserts (grow rehash)
        for (let i = 1; i < N; i += 7) map.set(i, i);                            // overwrites
        for (let i = 0; i < 256; i += 5) { map.delete("k" + i); map.set("k" + i, { i }); }
        if (!(gen % 20)) { map.clear(); set.clear(); for (let i = 0; i < N; ++i) { map.set(i, i); map.set("k" + (i & 255), { i: i & 255 }); set.add(i); } }
    }
    Atomics.store(box, "stop", 1);
    return gen;
});
const readers = []; for (let i = 0; i < 3; ++i) readers.push(new Thread(reader));
// The main thread reads a bounded number of rounds and then joins (GIL on, the
// other threads run while it blocks; GIL off everything above runs at once).
let mine = null;
for (let round = 0; round < 200 && !mine && !Atomics.load(box, "stop"); ++round)
    for (let k = 0; k < N && !mine; ++k) mine = readOnce(k);
const results = readers.map(t => t.join()); results.push(mine || "ok main");
const gens = writer.join();
if (typeof AMPLIFY_VERBOSE !== "undefined") print("writer generations " + gens + "; readers: " + results.join(", "));
for (const r of results) if (!String(r).startsWith("ok")) throw new Error("torn read: " + r);

// (2) The inline path: once sum() runs in the FTL, gets no longer reach the
// runtime reader (the DFG tier still calls it, so warm up well past FTL entry).
const quiet = new Map(); for (let i = 0; i < 1000; ++i) quiet.set(i, i * 2);
function sum(m, n) { let s = 0; for (let i = 0; i < n; ++i) s += m.get(i % 1000); return s; }
noInline(sum);
for (let i = 0; i < 2000; ++i) sum(quiet, 1000); // tier up
const GETS = 300000;
const before = $vm.jsThreadsCounter("mapReadLockFreeGILOff");
const s = sum(quiet, GETS);
const runtimeReads = $vm.jsThreadsCounter("mapReadLockFreeGILOff") - before;
let expected = 0; for (let i = 0; i < GETS; ++i) expected += (i % 1000) * 2;
if (s !== expected) throw new Error("sum " + s + " expected " + expected);
if (typeof AMPLIFY_VERBOSE !== "undefined") print("runtime reader calls for " + GETS + " FTL gets: " + runtimeReads);
if (!$vm.useThreadGIL() && runtimeReads > GETS / 20) throw new Error(runtimeReads + " of " + GETS + " Map.prototype.get calls from optimized code reached the runtime reader GIL off");
print("PASS");
