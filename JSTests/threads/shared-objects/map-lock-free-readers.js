//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOff
// GIL off, Map/Set has/get/size are validated lock-free reads (a seqlock on the
// owner, bumped by every writer under the table lock; SPEC-runtime Map/Set):
// readers no longer serialize on the table's cell lock. Checks (1) torn-read
// freedom: while a writer inserts, overwrites, deletes, clears and (through
// growth and shrink) rehashes a shared Map and Set, readers only ever observe,
// for a key k, "absent" or exactly the value the writer stores for k - never
// another key's value, a link, a half-built entry or a stale table's garbage;
// (2) read scaling: four threads reading one shared Map take well under the
// serialized time (before: 4 readers = 10x one reader GIL off).
load("../harness.js", "caller relative");

const KEYS = 2048;
const valueFor = (k, gen) => (typeof k === "number" ? k * 7 : k.length * 13) + gen * 100000;
const keys = []; for (let i = 0; i < KEYS; ++i) keys.push((i & 1) ? "s" + i : i);
const map = new Map();
const set = new Set();
const box = { stop: 0, gen: 0 };

const writer = new Thread(() => {
    let gen = 0;
    while (!Atomics.load(box, "stop")) {
        ++gen; box.gen = gen;
        for (const k of keys) { map.set(k, valueFor(k, gen)); set.add(k); }      // growth rehashes
        for (let i = 0; i < KEYS; i += 2) { map.delete(keys[i]); set.delete(keys[i]); } // tombstones, shrink rehashes
        for (let i = 0; i < KEYS; i += 4) map.set(keys[i], valueFor(keys[i], gen));
        if (gen % 8 === 0) { map.clear(); set.clear(); }
    }
    return gen;
});

function reader() {
    let seen = 0, bad = null;
    const deadline = Date.now() + 1500;
    while (Date.now() < deadline && !bad) {
        for (let i = 0; i < KEYS; ++i) {
            const k = keys[i];
            const v = map.get(k);
            if (v !== undefined) {
                ++seen;
                // value = base(k) + gen*100000 for SOME gen: check the residue.
                const base = valueFor(k, 0);
                if (typeof v !== "number" || (v - base) % 100000 !== 0 || v < base) { bad = "key " + k + " read " + String(v); break; }
            }
            const h = set.has(k);
            if (h !== true && h !== false) { bad = "has() returned " + String(h); break; }
            const sz = map.size;
            if (typeof sz !== "number" || sz < 0 || sz > KEYS) { bad = "size " + sz; break; }
        }
    }
    return bad || ("ok " + seen);
}
const readers = []; for (let i = 0; i < 3; ++i) readers.push(new Thread(reader));
const mine = reader();
const results = readers.map(t => t.join()); results.push(mine);
Atomics.store(box, "stop", 1);
const gens = writer.join();
if (typeof AMPLIFY_VERBOSE !== "undefined") print("writer generations " + gens + "; readers: " + results.join(", "));
for (const r of results) if (!String(r).startsWith("ok")) throw new Error("torn read: " + r);

// (2) read scaling on a quiescent shared map.
{
    const big = new Map(); for (let i = 0; i < 4096; ++i) { big.set(i, i * 2); big.set("k" + i, i); }
    const probe = []; for (let i = 0; i < 64; ++i) probe.push("k" + i);
    function work() { let s = 0; for (let r = 0; r < 150; ++r) for (let i = 0; i < 4096; ++i) { s += big.get(i); if (big.has(probe[i & 63])) s++; } return s; }
    let t0 = preciseTime(); const one = work(); const tOne = preciseTime() - t0;
    t0 = preciseTime(); const ths = [new Thread(work), new Thread(work), new Thread(work)]; const again = work(); for (const t of ths) if (t.join() !== one) throw new Error("sum mismatch"); const tFour = preciseTime() - t0;
    if (again !== one) throw new Error("sum mismatch");
    if (typeof AMPLIFY_VERBOSE !== "undefined") print("one reader " + (tOne * 1e3).toFixed(1) + " ms, four readers " + (tFour * 1e3).toFixed(1) + " ms");
    if (tFour > tOne * 3) throw new Error("four readers of one Map took " + (tFour / tOne).toFixed(1) + "x one reader (serialized reads?)");
}
print("PASS");
