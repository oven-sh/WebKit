//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-jit history §40 (eighth landing round). GIL off an OSR exit's jump is
// never repatched to its compiled ramp (no patching of reachable code outside
// a stop), so before this round EVERY exit of an already-compiled ramp went
// through the generation thunk and operationCompileOSRExit /
// operationCompileFTLOSRExit (all registers saved, the published-ramp lookup,
// all registers restored) - `gbemu` paid that about 25,000 times a run. Now
// DFG code dispatches exits through the JITData exit vector and FTL exit
// thunks read the published ramp pointer, so only an exit site's FIRST exit
// reaches the operation. The function below takes the same speculation exit
// a few hundred times (rare enough per call not to reoptimize at once);
// counted: exits that went through the operation vs ramps compiled.
// Before (GIL off): operations ~= exits taken (hundreds); after: operations
// == ramps compiled (a handful). GIL on / flag off repatch the jump, so the
// two counts were always equal there.
load("../harness.js", "caller relative");

function f(o, i) {
    let v = o.a;          // CheckStructure speculated on the common shape
    return v + (i & 3);
}
noInline(f);

const common = { a: 1 };
// Rare shapes: a fresh structure every 32 rare calls, so whatever set of
// shapes a recompile covers, later rare calls still fail its check and exit.
let rareSerial = 0, rare = null;
function nextRare() { if ((rareSerial++ & 31) === 0) { rare = { a: 2 }; rare["b" + rareSerial] = 3; } return rare; }
let sum = 0;
for (let i = 0; i < 200000; ++i) sum += f(common, i); // tier up on the common shape

const counter = (name) => $vm.jsThreadsCounter(name) || 0;
const opsBefore = counter("osrExitDFGOperation") + counter("osrExitFTLOperation");
const compilesBefore = counter("osrExitDFGCompile") + counter("osrExitFTLCompile");
let exitsProvoked = 0;
for (let i = 0; i < 400000; ++i) {
    if ((i & 1023) === 0) { sum += f(nextRare(), i); ++exitsProvoked; } // ~390 rare-shape calls
    else sum += f(common, i);
}
const ops = counter("osrExitDFGOperation") + counter("osrExitFTLOperation") - opsBefore;
const compiles = counter("osrExitDFGCompile") + counter("osrExitFTLCompile") - compilesBefore;
if (typeof sum !== "number" || sum !== sum) throw new Error("bad sum");
if (typeof AMPLIFY_VERBOSE !== "undefined") print("rare-shape calls: " + exitsProvoked + ", exit operations: " + ops + ", exit ramps compiled: " + compiles);
if (compiles < 1) throw new Error("no OSR exit ramp was compiled; the test did not exercise an exit");
// After: every exit past a site's first goes straight to the ramp, so the
// operation runs once per compiled ramp (plus a small allowance for exits
// racing a compile). Before, GIL off: once per exit taken.
if (ops > compiles + 8) throw new Error(ops + " exit operations for " + compiles + " compiled ramps: exits are still routed through the generation thunk");
print("PASS");
