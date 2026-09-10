//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-ungil history, eighth landing round ("loop entry into the superseded DFG
// code"). GIL off several threads run one function's Baseline code in a long
// loop (they were sent there when its optimized code was jettisoned) while an
// FTL function-entry replacement is installed over its DFG code. Those frames
// change tier only by loop OSR entry, which needs a DFG target, so every
// attempt failed ("target code block is not DFG"), was counted as an exit
// against the FTL code, and the threads stayed in Baseline - sharing one set
// of profiles and counters at four times the single-thread cost per operation
// - until enough failures jettisoned the FTL. installCode now keeps the DFG
// block the FTL superseded enterable at loops for such frames.
// The workload is the scaling suite's string-heavy at full scale, four
// threads. Before: about 1,900 refused loop entries a run at full scale and
// four threads at 0.9x of one; after: the frames enter the superseded DFG
// (counted), refusals are the rare case of that block itself being gone, and
// four threads run at 2.0-2.2x of one. Checked here: every thread computes the
// same checksum, and refusals stay rare.
load("../harness.js", "caller relative");

// Full scale in Release (the lock-out needs a run long enough for the FTL
// replacement to arrive while frames sit in Baseline); a Debug/ASAN build runs
// the same shape at a size it can finish.
const OUTER_ITERATIONS = $vm.assertEnabled && $vm.assertEnabled() ? 300 : 40000;
function workload() {
    const OUTER = OUTER_ITERATIONS;
    const pieces = [];
    for (let i = 0; i < 64; ++i) {
        let piece = "";
        const len = 3 + (i % 7);
        for (let j = 0; j < len; ++j) piece += String.fromCharCode(97 + ((i * 31 + j * 7) % 26));
        pieces.push(piece);
    }
    let hash = 0x811c9dc5 >>> 0;
    function mix(value) { hash = ((hash ^ (value & 0xffff)) * 0x01000193) >>> 0; }
    for (let o = 0; o < OUTER; ++o) {
        let rope = "";
        for (let i = 0; i < 700; ++i) rope += pieces[(i * 7 + o * 13) & 63];
        mix(rope.charCodeAt((o * 97) % rope.length));
        mix(rope.indexOf(pieces[(o * 5) & 63], (o * 11) % 512));
        mix(rope.length);
        const table = {};
        const span = rope.length - 9;
        for (let k = 0; k < 48; ++k) {
            const key = rope.substring((k * 53 + o * 17) % span, (k * 53 + o * 17) % span + 8);
            table[key] = (table[key] === undefined ? 0 : table[key]) + 1;
        }
        let distinct = 0, total = 0;
        for (const key in table) { distinct++; total += table[key]; }
        mix(distinct); mix(total);
    }
    return hash;
}

const reference = workload();
if (workload() !== reference) throw new Error("workload is not deterministic");

const refusedBefore = $vm.jsThreadsCounter("loopEntryRefusedReplacementIsFTLGILOff") || 0;
const enteredBefore = $vm.jsThreadsCounter("loopEntryIntoSupersededDFGGILOff") || 0;
const t0 = Date.now();
const threads = [];
for (let i = 0; i < 4; ++i) threads.push(new Thread(workload));
for (const t of threads) {
    const got = t.join();
    if (got !== reference) throw new Error("thread checksum " + got + " != " + reference + " (cross-thread interference)");
}
const elapsed = Date.now() - t0;
const refused = ($vm.jsThreadsCounter("loopEntryRefusedReplacementIsFTLGILOff") || 0) - refusedBefore;
const entered = ($vm.jsThreadsCounter("loopEntryIntoSupersededDFGGILOff") || 0) - enteredBefore;
if (typeof AMPLIFY_VERBOSE !== "undefined") print("four threads: " + elapsed + " ms; loop entries into the superseded DFG code: " + entered + "; refused because the replacement is FTL and no DFG code is enterable: " + refused);
// GIL on there is one running frame at a time and the FTL replacement is
// entered by the next call; the counters stay 0. GIL off, refusals happen only
// while no enterable DFG block exists - the recorded one was jettisoned, and
// none is recorded again until the FTL replacement is itself jettisoned by
// the failed-entry count (whose threshold grows with the thread count and the
// reoptimization back-off) and a new DFG-then-FTL pair installs: a quiet run
// sees 0-10, a run under the race amplifier on a loaded machine ~200, one in
// 500 on an overloaded one 650. Locked out as before the rule, every run
// refuses ~1,900 times at this scale and enters nothing.
if (refused > 1000) throw new Error(refused + " loop entries refused because the replacement is FTL (entries into the superseded DFG: " + entered + ")");
print("PASS");
