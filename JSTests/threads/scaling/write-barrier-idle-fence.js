//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// The shared heap's "mutator should be fenced" flag must follow marking on x86
// (raised by beginMarking, lowered by endMarking), not be raised at the moment
// the heap becomes shared: a raised flag sends every JIT write barrier through
// its store-load-fenced slow path, and before the sixth landing round a
// GIL-off process paid that from startup until its first collection finished
// (2.2x on this loop). Checks that a barrier-heavy put loop runs at the same
// speed before any collection as after one.
load("../harness.js", "caller relative");

function A(x) { this.x = x; this.o = null; }
function setx(o, v) { o.o = v; } // stores a cell: needs the write barrier
noInline(setx);
const objs = []; for (let i = 0; i < 512; ++i) objs.push(new A(i));
new Thread(() => 1).join(); // make sure the heap is shared in every mode

function run() {
    const t0 = preciseTime();
    for (let i = 0; i < 4e6; ++i) setx(objs[i & 511], objs[(i + 1) & 511]);
    return preciseTime() - t0;
}
// Tier up until the loop's time is stable, so that a loaded machine whose JIT
// threads lag does not put compile latency into "before" (seventh round: the
// fixed two warm-up runs measured 5x under a parallel test load, Debug).
let prev = run();
for (let i = 0; i < 12; ++i) { const t = run(); const stable = t > prev * 0.8 && t < prev * 1.25; prev = t; if (stable) break; }
const before = Math.min(run(), run());
fullGC();
const after = Math.min(run(), run());
if (typeof AMPLIFY_VERBOSE !== "undefined") print("put loop before first GC " + (before * 1e3).toFixed(1) + " ms, after " + (after * 1e3).toFixed(1) + " ms");
if (before > after * 1.6) throw new Error("write barriers before the first collection are " + (before / after).toFixed(1) + "x slower (fence raised while idle?)");
print("PASS");
