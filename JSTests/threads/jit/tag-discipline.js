//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// tag-discipline.js — SPEC-jit Task 13: I14 `validateButterflyTagDiscipline`
// corpus + I21 poll-placement run.
//
// The actual validation is C++-side: with --validateButterflyTagDiscipline=1
// (M1 option; run-jit-tests.sh probes for it and adds it when present) the
// DFG/B3 pass asserts every generated butterfly dereference (a) masks the
// tag, (b) is proven tag-zero by the IC, or (c) is inline-cell; the I21
// extension asserts every poll is immediately followed by an invalidation
// point and no poll sits in an IC fast-path window (I16). This file's job is
// to push representative butterfly-heavy code through ALL tiers flag-on so
// the validator sees every §5.5 emission family:
//   - GetButterfly / PutByOffset OOL (DFG+FTL twins, E1/E2 paths)
//   - array element read/write (contiguous + ArrayStorage)
//   - spread / sort / enumerator (the auxiliary I14 inventory rows)
//   - inlined packed self-word fast paths (§4.2)
//
// Also doubles as the Task-9/10 exit-origin audit vehicle: GetButterfly/
// PutByOffset now emit speculation checks flag-on; any exitOK==false
// placement asserts at emission while this corpus compiles.

load("../harness.js", "caller relative");

function oolGet(o) { return o.q7; }
noInline(oolGet);

function oolPut(o, v) { o.q7 = v; }
noInline(oolPut);

function makeFat(seed) {
    const o = {};
    for (let i = 0; i < 12; ++i)
        o["q" + i] = seed + i;
    return o;
}

function arrayRW(a, i, v) {
    const x = a[i];
    a[i] = v;
    return x;
}
noInline(arrayRW);

function spreadIt(a) { return [...a]; }
noInline(spreadIt);

function sortIt(a) { return a.slice().sort((x, y) => x - y); }
noInline(sortIt);

function enumerate(o) {
    let sum = 0;
    for (const k in o)
        sum += o[k];
    return sum;
}
noInline(enumerate);

const fats = [makeFat(0), makeFat(100)];
const contiguous = [5, 3, 8, 1, 9, 2, 7, 4];
const asArray = [5, 3, 8, 1];
if (typeof $vm !== "undefined" && $vm.ensureArrayStorage)
    $vm.ensureArrayStorage(asArray);

let sink = 0;
for (let i = 0; i < 200000; ++i) {
    const fat = fats[i & 1];
    sink += oolGet(fat);
    oolPut(fat, i & 0xff);
    sink += arrayRW(contiguous, i & 7, i & 15);
    sink += arrayRW(asArray, i & 3, i & 15);
    if (!(i & 0x3ff)) {
        sink += spreadIt(contiguous).length;
        sink += sortIt(contiguous)[0];
        sink += enumerate(fat);
    }
}

// Tier sanity: by now the hot functions should have left the LLInt. We do
// not hard-assert FTL (machine/test-config dependent), but record it.
if (typeof $vm !== "undefined" && $vm.dfgTrue) {
    function tierProbe() { return $vm.dfgTrue(); }
    noInline(tierProbe);
    let sawDFG = false;
    for (let i = 0; i < 100000 && !sawDFG; ++i)
        sawDFG = tierProbe();
    print("tag-discipline: dfg reached = " + sawDFG);
}

shouldBeTrue(sink > 0);
print("tag-discipline: PASS (validator ran over the corpus)");
