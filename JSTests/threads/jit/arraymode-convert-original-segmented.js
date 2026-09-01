//@ requireOptions("--useJSThreads=1", "--useConcurrentJIT=0", "--useFTLJIT=0", "--thresholdForJITAfterWarmUp=50", "--thresholdForOptimizeAfterWarmUp=200")
// A put_by_val site profiled on original Int32 arrays that stores an object
// compiles as Contiguous/OriginalArray/Convert: ArrayifyToStructure to the
// original ArrayWithContiguous structure, then the indexed store. A foreign
// push past the vector length segments an array in place and keeps its
// original structure, and the flag-on Int32->Contiguous relabel keeps the
// segmented spine, so Arrayify leaves a segmented butterfly behind. After the
// first BadIndexingType exit the recompile must keep the segmented-butterfly
// bit for this mode (the exit site is the only signal for a put), otherwise
// the CodeBlock exits and reoptimizes until it gives up.

function check(cond, msg) { if (!cond) throw new Error(msg); }

if (typeof numberOfDFGCompiles !== "function" || typeof Thread !== "function") {
    print("SKIP: needs jsc shell with Thread + numberOfDFGCompiles");
    quit(0);
}

function makeInt(i) { return [i, i, i, i]; }
noInline(makeInt);
function makeObj(i) { return [{}, i, i, i]; }
noInline(makeObj);

function store(a, v) { a[0] = v; }
noInline(store);

var shared = makeInt(1);
var t = new Thread(() => { for (var i = 0; i < 64; i++) shared.push(i); return shared.length; });
check(t.join() === 68, "foreign push did not run");

// Baseline compiles while only Contiguous arrays are seen, the DFG while only
// Int32 arrays are seen: the profile prunes to Int32 at the first DFG compile
// and the object value selects the Convert mode.
for (var i = 0; i < 100; i++)
    store(makeObj(i), {});
for (var i = 0; i < 300; i++)
    store(makeInt(i), {});

var value = { tag: "stored" };
for (var i = 0; i < 60000; i++)
    store(shared, value);

check(shared[0] === value, "shared[0] lost the store");
check(shared.length === 68, "shared.length changed: " + shared.length);
var compiles = numberOfDFGCompiles(store);
check(compiles >= 1, "store never reached DFG");
check(compiles <= 3, "store exit/reoptimize loop on a segmented original array: " + compiles + " DFG compiles");
