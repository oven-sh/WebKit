//@ requireOptions("--useJSThreads=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=50", "--thresholdForOptimizeAfterWarmUp=200", "--thresholdForFTLOptimizeAfterWarmUp=1000")
// SCALEBENCH §45 / MAP-BIMODAL-EVIDENCE.md root cause (g): a foreign-TID
// (worker) lazy-reification of a constructor static property is a structure
// transition on a Flat butterfly the main thread allocated. Pre-§45 that
// always converted (§4.2) and the constructor stayed Segmented for life;
// DFG compileGetButterfly's segmented predicate then routed to
// speculationCheck(BadIndexingType), and DFGByteCodeParser::handleGetById
// did NOT consult hasExitSite(BadIndexingType), so the recompile re-emitted
// CheckStructure+GetButterfly+GetByOffset every time -> 8-15x DFG compiles
// for a hot get_by_id on String/Array/Object (the §44 intcs W=16 slow-mode).
//
// §45 closes the hazard two ways and this test pins both:
//   (a) ConcurrentButterfly §4.2 noseg-property-only StayFlatShared gate:
//       a foreign/SW property transition on a property-only Flat butterfly
//       with NO capacity growth reuses the live flat allocation under the
//       cell lock and publishes (installerTID, SW=1) instead of segmenting.
//       String (3/4 -> 4/4) and Object (14/16 -> 15/16) hit this gate; the
//       get_by_id below should compile ONCE (no BadIndexingType exit at all).
//   (b) DFGByteCodeParser::handleGetById BadIndexingType backstop: when the
//       transition DOES grow capacity (Array 8/8 -> 9/16) the gate cannot
//       apply and the constructor still segments. The first DFG compile
//       OSR-exits BadIndexingType once; the recompile sees the exit site and
//       falls back to the GetById IC node -> converges in TWO compiles, not
//       a loop. The function still reaches FTL.
//
// GIL-on this test is a no-op tier-up exercise (the foreign-transition path
// is not reached at scale and the gate is GIL-off-only); GIL-off it pins the
// engine-side fix that retired the §44 bench prewarm.

function check(cond, msg) { if (!cond) throw new Error(msg); }

if (typeof numberOfDFGCompiles !== "function" || typeof Thread !== "function") {
    print("SKIP: needs jsc shell with Thread + numberOfDFGCompiles");
    quit(0);
}

// Step 1: worker-thread lazy reification (foreign TID). Touch the SAME three
// properties the §44 residual named: Array.from, Object.keys, String.raw.
// String.fromCharCode covered by String.raw (same constructor butterfly).
let w = new Thread(function () { Array.from; Object.keys; String.raw; });
w.join();

// Step 2: hot get_by_id on each constructor. noInline so each is its own DFG
// CodeBlock; the loop body is just the property load + type tag so the DFG
// graph is CheckStructure(ctor) + GetButterfly + GetByOffset(k) + branch.
function hotString() { return typeof String.raw; }
noInline(hotString);
function hotObject() { return typeof Object.keys; }
noInline(hotObject);
function hotArray() { return typeof Array.from; }
noInline(hotArray);

let N = 50000;
for (let i = 0; i < N; ++i) {
    if (hotString() !== "function") throw new Error("String.raw wrong");
    if (hotObject() !== "function") throw new Error("Object.keys wrong");
    if (hotArray() !== "function") throw new Error("Array.from wrong");
}

// Step 3: convergence bound. Under useConcurrentJIT=0 + the pinned thresholds
// each function compiles to DFG, exits at most once (Array), recompiles, then
// tiers to FTL. numberOfDFGCompiles counts DFG+FTL together; pre-§45 the
// String case alone was 8-15. A bound of 4 admits {DFG, DFG-after-exit, FTL,
// FTL-after-exit} and is comfortably below the loop signature.
let nS = numberOfDFGCompiles(hotString);
let nO = numberOfDFGCompiles(hotObject);
let nA = numberOfDFGCompiles(hotArray);
print("numberOfDFGCompiles: String=" + nS + " Object=" + nO + " Array=" + nA);

check(nS >= 1, "hotString never reached DFG");
check(nS <= 4, "hotString recompile loop (StayFlatShared gate broken): " + nS);
check(nO >= 1, "hotObject never reached DFG");
check(nO <= 4, "hotObject recompile loop (StayFlatShared gate broken): " + nO);
check(nA >= 1, "hotArray never reached DFG");
check(nA <= 4, "hotArray recompile loop (handleGetById backstop broken): " + nA);

// S45-DUPLICATE-PROPERTY-NAME gate (folded here; the named
// JSTests/threads/objectmodel/reify-static-idempotent.js path is outside this
// task's owned file set). The §45 residual #2 `describe()` "duplicates" were
// the PRIVATE builtin symbols (`@hasOwn`, `@keys`, `@from`) rendered without
// the `@` distinguisher — distinct uids. The structural invariant we actually
// care about is: after worker-first foreign reification, the spec-observable
// own-string-property list contains no duplicate name (a same-uid double
// install would surface here). Hammer a second round of worker reifies first
// to exercise the under-lock idempotency re-probe in reifyStaticProperty.
let w2 = new Thread(function () {
    Array.from; Object.keys; String.raw; Object.hasOwn; Array.isArray; String.fromCharCode;
});
w2.join();
function assertNoDuplicateOwnNames(ctor, label) {
    let names = Object.getOwnPropertyNames(ctor);
    let seen = new Set();
    for (let n of names) {
        if (seen.has(n))
            throw new Error("S45: duplicate own property name '" + n + "' on " + label + " (own names: " + names.join(",") + ")");
        seen.add(n);
    }
    // The reified statics must each appear exactly once as an own string key.
    return names;
}
let oNames = assertNoDuplicateOwnNames(Object, "Object");
let sNames = assertNoDuplicateOwnNames(String, "String");
let aNames = assertNoDuplicateOwnNames(Array, "Array");
check(oNames.filter(n => n === "keys").length === 1, "Object.keys not exactly-once own: " + oNames);
check(oNames.filter(n => n === "hasOwn").length === 1, "Object.hasOwn not exactly-once own: " + oNames);
check(aNames.filter(n => n === "from").length === 1, "Array.from not exactly-once own: " + aNames);
check(sNames.filter(n => n === "raw").length === 1, "String.raw not exactly-once own: " + sNames);
print("S45 reify-static-idempotent: Object=" + oNames.length + " String=" + sNames.length + " Array=" + aNames.length + " own names, no duplicates");
