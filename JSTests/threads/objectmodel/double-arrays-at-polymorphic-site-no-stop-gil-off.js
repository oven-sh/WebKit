//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-jit history §42 / OM §4.7 (eighth landing round). A get-by-val site
// that meets Double arrays among Contiguous ones was compiled with a
// converting Contiguous array mode (`Arrayify`), which flag-off rewrites a
// Double array in place but GIL off is a stop-the-world per Double array
// (`Basic`: up to 840,000 stops a run from one site). GIL off such a site is
// now generic (inline-cached per shape, converting nothing). Counted here:
// "OM relabel Double->Contiguous" stop requests while a hot site reads a
// stream of fresh one-element arrays, half of them Double.
// Before (GIL off): about one stop per Double array (~50,000); after: none
// from this site. The sums are checked in every mode.
load("../harness.js", "caller relative");

function lastOf(a) { return a[a.length - 1]; } // the polymorphic read site
noInline(lastOf);
// Double arrays from a source with no allocation profile to learn from
// (JSON.parse builds them in C++), so the site keeps meeting Double arrays -
// as `Basic` and the sjcl tests do - instead of the allocation site demoting
// itself to Contiguous after the first conversions (OM T4-P).
function make(i) { return (i & 1) ? JSON.parse((i & 2) ? "[0.5]" : "[1.5, 2.5]") : ["s" + i]; }
noInline(make);

let acc = 0;
for (let i = 0; i < 20000; ++i) { const v = lastOf(make(i)); acc += typeof v === "number" ? v : 1; } // warm both shapes

const stopsBefore = $vm.jsThreadsCounter("OM relabel Double->Contiguous") || 0;
const requestsBefore = $vm.jsThreadsCounter("stwRequest");
const N = 100000;
for (let i = 0; i < N; ++i) { const v = lastOf(make(i)); acc += typeof v === "number" ? v : 1; }
const relabelStops = ($vm.jsThreadsCounter("OM relabel Double->Contiguous") || 0) - stopsBefore;
const requests = $vm.jsThreadsCounter("stwRequest") - requestsBefore;

// The array intrinsics at such a site (push/pop/indexOf are inlined only for
// JSArray-class modes; the generic mode must not claim that class - the
// GIL-off stress suite found the DFG compiling ArrayPush without storage).
function pushPop(a, v) { a.push(v); const w = a.pop(); return a.indexOf(w) + (w === v ? 1 : 0); }
noInline(pushPop);
for (let i = 0; i < 60000; ++i) {
    const a = make(i);
    const r = pushPop(a, (i & 1) ? 9.5 : "z");
    if (r !== 0) throw new Error("pushPop on " + $vm.indexingMode(a) + " returned " + r);
}

let expected = 0;
const term = (i) => (i & 1) ? ((i & 2) ? 0.5 : 2.5) : 1;
for (let i = 0; i < 20000; ++i) expected += term(i);
for (let i = 0; i < N; ++i) expected += term(i);
if (acc !== expected) throw new Error("bad sum " + acc + " expected " + expected);
if (typeof AMPLIFY_VERBOSE !== "undefined") print("Double arrays read: " + N / 2 + ", Double->Contiguous relabel stops: " + relabelStops + ", stop requests: " + requests);
if (!$vm.useThreadGIL() && relabelStops > 100) throw new Error(relabelStops + " Double->Contiguous stop-the-world relabels for " + N / 2 + " Double arrays read at one site");
print("PASS");
