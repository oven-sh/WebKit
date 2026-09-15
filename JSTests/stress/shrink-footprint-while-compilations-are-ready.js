//@ runDefault("--thresholdForJITAfterWarmUp=5", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--thresholdForJITAfterWarmUp=5", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--thresholdForJITAfterWarmUp=5", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50", "--forceCodeBlockToJettisonDueToOldAge=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--thresholdForJITAfterWarmUp=5", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50", "--useConcurrentJIT=0")

// Code is dropped and decoded again round after round while compilations of it are in flight and ready to be installed,
// keeping the code in use (VM::ShrinkFootprint::KeepCodeInUse) in every other round, and every third time through
// Heap::deleteAllUnlinkedCodeBlocks directly, where nothing has finished the compiler threads' plans beforehand:
// finishing them allocates, which must not happen once the heap has been prepared for iteration. With a persistent
// bytecode cache the drops are real; forceCodeBlockToJettisonDueToOldAge makes them real when the code in use is kept.
function assert(c, m) { if (!c) { print("FAIL: " + m); $vm.abort(); } }
var rounds = 10;
var warm = Math.min(testLoopCount, 100);

function makeKit(seed) {
    let state = seed;
    function inc(a) { state += a; return state; }
    function hot(a, b) { let s = 0; for (let i = 0; i < a; ++i) s += inc(b) & 7; return s; }
    function sw(a) { switch (a) { case 0: return "zero"; case 1: return "one"; case 2: return "two"; case 3: return "three"; case 4: return "four"; case 5: return "five"; default: return "n" + a; } }
    function ssw(a) { switch (a) { case "a": return 1; case "b": return 2; case "cc": return 3; default: return 0; } }
    function tc(a) { try { if (a & 1) throw new Error("odd" + a); return a; } catch (e) { return e.message.length; } finally { state++; } }
    function stk() { return new Error("here").stack; }
    function* gen(n) { for (let i = 0; i < n; ++i) { let f = (x) => x + i + state; yield f(i); } }
    async function af(p) { let a = inc(1); let v = await p; return a + v + inc(1); }
    class C { #p = 1; constructor(v) { this.v = v; } get g() { return this.v + this.#p; } static s(v) { return new C(v).g; } m() { return () => this.v; } }
    function mk(i) { return function made(x) { return x * i + state; }; }
    function arr(a) { return [a, a + 1, a + 2].map((x) => x * 2).filter((x) => x % 3).reduce((p, c) => p + c, 0); }
    function re(s) { return /a(b+)c/.exec(s)?.[1]?.length ?? -1; }
    function tmpl(a) { return `v=${a}:${sw(a)}`; }
    function obj(a) { return { a, b: a + 1, c: { d: a }, [a]: 1 }; }
    function big(a) { let o = obj(a); return o.a + o.b + o.c.d + Object.keys(o).length; }
    return { inc, hot, sw, ssw, tc, stk, gen, af, C, mk, arr, re, tmpl, big };
}

function exercise(k, n) {
    let out = [];
    for (let i = 0; i < n; ++i) {
        out.length = 0;
        out.push(k.hot(10, 3), k.sw(i % 8), k.ssw(["a", "b", "cc", "d"][i & 3]), k.tc(i), k.C.s(i), new k.C(i).m()(), k.mk(i)(2), k.arr(i), k.re("xabbbc"), k.tmpl(i % 7), k.big(i));
    }
    return out.join("|");
}
function normStack(s) { return s.split("\n").slice(0, 1).map((l) => l.replace(/^.*\//, "")).join(";"); }
function fresh(n) { return exercise(makeKit(0), n); }

const expected = fresh(warm);
const kitA = makeKit(0);
assert(exercise(kitA, warm) === expected, "kitA initial");
const stackBefore = normStack(kitA.stk());
const toStringBefore = kitA.hot.toString() + kitA.C.toString() + kitA.gen.toString();
const g = kitA.gen(1000);
g.next();
let pending = [];
let round = 0;
const kits = [kitA];

function step() {
    try {
        if (round) {
            if (round & 1) fullGC();
            // old closures (made before every drop so far)
            const kOld = kits[round % kits.length];
            kOld.sw(3); kOld.hot(3, 1);
            assert(normStack(kitA.stk()) === stackBefore, "stack changed: " + normStack(kitA.stk()) + " vs " + stackBefore);
            assert(kitA.hot.toString() + kitA.C.toString() + kitA.gen.toString() === toStringBefore, "toString changed");
            const v = g.next();
            assert(!v.done && typeof v.value === "number", "generator");
            // a new kit from the (maybe re-decoded) parent behaves the same as one that never saw a drop
            assert(fresh(round % 3 ? 30 : warm) === (round % 3 ? fresh30 : expected), "fresh kit after drop " + round);
            kits.push(makeKit(round));
            exercise(kits[kits.length - 1], round % 4 ? 5 : warm);
            if (kits.length > 6) kits.splice(1, 1);
            for (const [p, open, want] of pending) open(5);
            pending = [];
            kitA.hot(1,1); $vm.codeBlockFor(kitA.hot);
        }
        if (round++ >= rounds)
            return;
        let open; const pr = new Promise((r) => { open = r; });
        const res = kitA.af(pr); res.then((v) => assert(typeof v === "number" && v === v, "async result"));
        pending.push([res, open]);
        const keepInUse = !!(round & 1);
        if (jscOptions().forceCodeBlockToJettisonDueToOldAge)
            fullGC();
        if (round % 3 === 2)
            $vm.returnCodeToBytecodeCacheWhenIdle(keepInUse);
        else
            $vm.shrinkFootprintWhenIdle(true, keepInUse);
        setTimeout(step, 0);
    } catch (e) { print("FAIL: " + e + "\n" + e.stack); $vm.abort(); }
}
const fresh30 = fresh(30);
step();
