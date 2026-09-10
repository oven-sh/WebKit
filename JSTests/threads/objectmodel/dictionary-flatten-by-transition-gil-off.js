//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-jit history §38 (eighth landing round). GIL off, flattening a
// dictionary is a structure-only transition to a fresh non-dictionary clone
// (same table, same offsets, same storage, new StructureID), so the inline
// cache paths that used to refuse dictionaries on a prototype chain forever
// now flatten once and cache. (1) Values: a prototype that went dictionary
// (adds and deletes) is read through by four threads over several shapes while
// a mutator keeps replacing values on it; every read returns a value the
// property held (never another property's, never a torn one), before and
// after the flatten, and a reader that still holds the pre-flatten structure
// reads the same slots. (2) The flatten happens and the site caches: a
// single-threaded loop reading a prototype property through a dictionary
// prototype no longer reaches the generic/gave-up operations once per access
// (before: 100000 of 100000 GIL off; after: a handful), and the flatten
// counter moved.
load("../harness.js", "caller relative");

function makeDictionaryProto(tag) {
    const p = { kind: tag, a: "a-" + tag, b: "b-" + tag, c: "c-" + tag };
    for (let i = 0; i < 40; ++i) p["tmp" + i] = i;
    $vm.toUncacheableDictionary(p);                      // what a long enough run of deletes does to a prototype
    for (let i = 0; i < 40; ++i) delete p["tmp" + i];   // holes in the table, kept across the flatten GIL off
    return p;
}
const P = makeDictionaryProto("P");
const SHAPES = 6;
function makeObj(i) { const o = Object.create(P); for (let k = 0; k <= i; ++k) o["own" + k] = k; o.tag = i; return o; }
const objs = []; for (let i = 0; i < SHAPES; ++i) objs.push(makeObj(i));

function readA(o) { return o.a; }
function readKind(o) { return o.kind; }
function hasB(o) { return "b" in o; }
noInline(readA); noInline(readKind); noInline(hasB);
const aValues = new Set(["a-P", "a-1", "a-2", "a-3"]);

function checkOnce(n) {
    for (let i = 0; i < objs.length; ++i) {
        const o = objs[i];
        const a = readA(o);
        if (!aValues.has(a)) return "shape " + i + " a=" + String(a);
        const k = readKind(o);
        if (k !== "P") return "shape " + i + " kind=" + String(k);
        const h = hasB(o);
        if (h !== true) return "shape " + i + " has b=" + String(h);
        if (o.tag !== i) return "shape " + i + " tag=" + String(o.tag);
    }
    return null;
}
noInline(checkOnce);

const box = { stop: 0 };
function worker() {
    let bad = null, n = 0;
    for (let round = 0; round < 20000 && !bad && !Atomics.load(box, "stop"); ++round, ++n) bad = checkOnce(n);
    return bad || ("ok " + n);
}
const mutator = new Thread(() => {
    let n = 0;
    for (; n < 20000 && !Atomics.load(box, "stop"); ++n) {
        P.a = "a-" + (1 + n % 3);          // replace on the dictionary / flattened prototype
        if (!(n % 501)) { P["extra" + n] = n; delete P["extra" + n]; } // may re-dictionary it after the flatten
    }
    P.a = "a-P";
    return n;
});
const workers = []; for (let i = 0; i < 3; ++i) workers.push(new Thread(worker));
let mainBad = null;
for (let round = 0; round < 10000 && !mainBad && !Atomics.load(box, "stop"); ++round) mainBad = checkOnce(round);
const steps = mutator.join();
Atomics.store(box, "stop", 1);
const results = workers.map(t => t.join()); results.push(mainBad || "ok main");
if (typeof AMPLIFY_VERBOSE !== "undefined") print("mutator steps " + steps + "; workers: " + results.join(", "));
for (const r of results) if (!String(r).startsWith("ok")) throw new Error("bad read: " + r);
if (readA(objs[0]) !== "a-P") throw new Error("final a: " + readA(objs[0]));

// (2) One thread, a fresh dictionary prototype, one get site through it.
const Q = makeDictionaryProto("Q");
const qobjs = []; for (let i = 0; i < SHAPES; ++i) { const o = Object.create(Q); o["own" + i] = i; qobjs.push(o); }
function readC(o) { return o.c; }
noInline(readC);
function loop(n) { let s = 0; for (let k = 0; k < n; ++k) if (readC(qobjs[k % qobjs.length]) === "c-Q") ++s; return s; }
noInline(loop);
const flattensBefore = $vm.jsThreadsCounter("dictionaryFlattenByTransition");
for (let i = 0; i < 200; ++i) loop(500);
const counters = ["icGetByIdGeneric", "icGetByIdGaveUp", "icGetByIdOptimize"];
const before = counters.map(c => $vm.jsThreadsCounter(c));
const ACCESSES = 100000;
const s = loop(ACCESSES);
const slow = counters.map((c, i) => $vm.jsThreadsCounter(c) - before[i]);
const flattens = $vm.jsThreadsCounter("dictionaryFlattenByTransition") - flattensBefore;
if (s !== ACCESSES) throw new Error("loop " + s);
const total = slow.reduce((a, b) => a + b, 0);
if (typeof AMPLIFY_VERBOSE !== "undefined") print("slow-path gets for " + ACCESSES + " accesses through a dictionary prototype: " + total + " (" + counters.map((c, i) => c + "=" + slow[i]).join(" ") + "); flattens by transition: " + flattens);
if (!$vm.useThreadGIL()) {
    if (total > ACCESSES / 20) throw new Error(total + " of " + ACCESSES + " gets through a dictionary prototype took a slow path GIL off (" + counters.map((c, i) => c + "=" + slow[i]).join(" ") + ")");
    if (flattens < 1) throw new Error("no flatten-by-transition happened GIL off");
}
print("PASS");
