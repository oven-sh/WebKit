//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-jit history §37 (eighth landing round). GIL off, every JS thread owns a
// megamorphic cache and invalidations bump one process-wide epoch, so property
// sites that see more shapes than an inline cache holds use the cache again
// instead of the generic path on every access. (1) Values: four threads read,
// test and write through megamorphic sites over forty shapes while another
// thread rewrites and deletes properties of the shared prototype and swaps one
// object's prototype; every read is a value the property held at some point
// (never another property's value, never a torn one), and once the mutator is
// joined every thread's sites see its final state (an invalidation ordered
// before a read by happens-before is never missed). (2) The cache is real: a
// single-threaded megamorphic get loop no longer reaches the generic or
// gave-up operations once per access (before: 200000 of 200000 GIL off;
// after: the cache misses only, a few hundred).
load("../harness.js", "caller relative");

const SHAPES = 40;
const protoValues = ["p0", "p1", "p2", "p3"];
const P = { shared: "p0", gone: "g", depth: "P" };
const Q = { shared: "p3", depth: "Q" }; // an alternative prototype; no `gone`

function makeShape(i, proto) {
    const o = Object.create(proto);
    // Distinct insertion orders => distinct structures; `v` and `tag` always own.
    for (let k = 0; k < (i % 5); ++k) o["pad" + k + "_" + i] = k;
    o.tag = i; o.v = i * 10; o["own" + i] = i;
    return o;
}
const shared = []; for (let i = 0; i < SHAPES; ++i) shared.push(makeShape(i, P));

function getV(o) { return o.v; }
function getShared(o) { return o.shared; }
function getGone(o) { return o.gone; }
function hasGone(o) { return "gone" in o; }
function putW(o, x) { o.w = x; }
noInline(getV); noInline(getShared); noInline(getGone); noInline(hasGone); noInline(putW);

function checkOnce(objs, mine, n) {
    for (let i = 0; i < objs.length; ++i) {
        const o = objs[i];
        const v = getV(o);
        if (v !== o.tag * 10) return "shape " + i + " v " + String(v);
        const s = getShared(o);
        if (!protoValues.includes(s)) return "shape " + i + " shared " + String(s);
        const g = getGone(o);
        if (g !== "g" && g !== undefined) return "shape " + i + " gone " + String(g);
        const h = hasGone(o);
        if (typeof h !== "boolean") return "shape " + i + " has " + typeof h;
        const m = mine[i];
        putW(m, n + i);                       // replace after the first round, transition on the first
        if (m.w !== n + i) return "shape " + i + " own write read back " + String(m.w);
    }
    return null;
}
noInline(checkOnce);

const box = { stop: 0 };
function worker() {
    const mine = []; for (let i = 0; i < SHAPES; ++i) mine.push(makeShape(i, P));
    let bad = null, n = 0;
    for (let round = 0; round < 3000 && !bad && !Atomics.load(box, "stop"); ++round, n += SHAPES)
        bad = checkOnce(shared, mine, n);
    return bad || ("ok " + n);
}

const mutator = new Thread(() => {
    let n = 0;
    for (; n < 4000 && !Atomics.load(box, "stop"); ++n) {
        P.shared = protoValues[n & 3];                 // replace on a prototype
        if (n & 1) delete P.gone; else P.gone = "g";   // delete / re-add on a prototype (dictionary after the first delete)
        if (!(n % 97)) Object.setPrototypeOf(shared[n % SHAPES], (n / 97) & 1 ? Q : P); // prototype swap of a base
    }
    // Final state every thread must observe after joining this one.
    P.shared = "p2"; delete P.gone; Q.shared = "p2";
    return n;
});
const workers = []; for (let i = 0; i < 3; ++i) workers.push(new Thread(worker));
const mineMain = []; for (let i = 0; i < SHAPES; ++i) mineMain.push(makeShape(i, P));
let mainBad = null;
for (let round = 0, n = 0; round < 1500 && !mainBad && !Atomics.load(box, "stop"); ++round, n += SHAPES)
    mainBad = checkOnce(shared, mineMain, n);
const steps = mutator.join();
Atomics.store(box, "stop", 1);
const results = workers.map(t => t.join()); results.push(mainBad || "ok main");
if (typeof AMPLIFY_VERBOSE !== "undefined") print("mutator steps " + steps + "; workers: " + results.join(", "));
for (const r of results) if (!String(r).startsWith("ok")) throw new Error("bad read: " + r);
// Happens-after the mutator (join): the final state, through the same sites, on
// this thread and on a fresh one.
function finalCheck() {
    for (let i = 0; i < SHAPES; ++i) {
        if (getShared(shared[i]) !== "p2") return "final shared of shape " + i + " is " + String(getShared(shared[i]));
        if (getGone(shared[i]) !== undefined) return "final gone of shape " + i + " is " + String(getGone(shared[i]));
        if (hasGone(shared[i]) !== false) return "final has of shape " + i;
    }
    return "ok";
}
const finalMain = finalCheck();
const finalOther = new Thread(finalCheck).join();
if (finalMain !== "ok" || finalOther !== "ok") throw new Error("stale after join: " + finalMain + " / " + finalOther);

// (2) One thread, one megamorphic get site over forty stable shapes.
const stable = []; for (let i = 0; i < SHAPES; ++i) stable.push(makeShape(i, { base: 1 }));
function sumV(objs, n) { let s = 0; for (let k = 0; k < n; ++k) s += getV(objs[k % objs.length]); return s; }
noInline(sumV);
for (let i = 0; i < 300; ++i) sumV(stable, 1000);
const counters = ["icGetByIdMegamorphicMiss", "icGetByIdGaveUp", "icGetByIdGeneric"];
const before = counters.map(c => $vm.jsThreadsCounter(c));
const ACCESSES = 200000;
const s = sumV(stable, ACCESSES);
const slow = counters.map((c, i) => $vm.jsThreadsCounter(c) - before[i]);
let expected = 0; for (let k = 0; k < ACCESSES; ++k) expected += (k % SHAPES) * 10;
if (s !== expected) throw new Error("sum " + s + " expected " + expected);
const total = slow.reduce((a, b) => a + b, 0);
if (typeof AMPLIFY_VERBOSE !== "undefined") print("slow-path gets for " + ACCESSES + " megamorphic accesses: " + total + " (" + counters.map((c, i) => c + "=" + slow[i]).join(" ") + ")");
if (!$vm.useThreadGIL() && total > ACCESSES / 20) throw new Error(total + " of " + ACCESSES + " megamorphic gets took a slow path GIL off (" + counters.map((c, i) => c + "=" + slow[i]).join(" ") + ")");
print("PASS");
