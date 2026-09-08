//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel T4-P (seventh landing round): GIL off, DFG code that
// allocates an array from a Double allocation profile reports the allocation
// to the profile and, when the previous array recorded there has left Double,
// calls out to demote the profile. The array lives in a DFG temporary across
// that call; the first implementation let the call clobber it, so the node's
// result was a garbage pointer whenever the slow path ran (found by the
// stress suite and the mirror harness, not by the T4-P test: it needs an
// allocation site whose arrays keep flipping between Double and Contiguous,
// here [x, x|0] with x sometimes NaN, which a Double array cannot hold).
// Single-threaded and two-threaded; every element is checked. GIL on / flag
// off never take the path and run the same checks.
load("../harness.js", "caller relative");

function make(view) { const x = view.getFloat64(0); return [x, x | 0]; } // the stress test's shape (register pressure decides which register holds the array)
noInline(make);

function run(seed) {
    const view = new DataView(new ArrayBuffer(8));
    let bad = 0;
    for (let i = 0; i < 300000; ++i) {
        const x = (i > 10000 && (i + seed) % 2053 === 0) ? NaN : (i & 1023) + 0.5; // NaN rarely and late: the site is compiled Double first, then its arrays leave Double now and then
        view.setFloat64(0, x);
        const a = make(view);
        if (a.length !== 2) { bad++; continue; }
        if (x !== x) { if (a[0] === a[0] || a[1] !== 0) bad++; }
        else if (a[0] !== x || a[1] !== (x | 0)) bad++;
    }
    return bad;
}
let bad = run(0);
const t = new Thread(() => run(3));
bad += run(5);
bad += t.join();
if (bad) throw new Error(bad + " arrays came back wrong from the Double-profile allocation site");
print("PASS");
