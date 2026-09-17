//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
//@ threadsRequireGILOff
// SPEC-jit §5.5, history §51 (tenth round). With the shared heap C++ rounds every
// fresh contiguous vector length up to 4k-1; generated code that allocated an
// array of a length unknown at compile time kept the raw length, so it drew
// from a size class the C++ slow path never allocates from and therefore never
// refills: `new Array(n)` with n = 4, `a.slice(0, 4)` and `a.map(f)` over four
// elements called the generic operation for every allocation (355, 440 and 623
// instructions a call against 98, 165 and 209 with the GIL on). The DFG and the
// FTL now round as C++ does. The test counts the operation's calls once the
// functions are compiled: one per refilled block (a few per cent, more when
// collections are frequent; 29 % on a loaded Debug build), not one per allocation
// (300,000 of 300,000 before).
// On a spawned thread too, whose allocators are its own.
load("../harness.js", "caller relative");

function newDynamic(n) { return new Array(n); }
function sliceFour(a) { return a.slice(0, 4); }
function mapFour(a) { return a.map(x => x + 1); }
noInline(newDynamic);
noInline(sliceFour);
noInline(mapFour);

function exercise() {
    const source = [1, 2, 3, 4, 5, 6, 7, 8];
    const four = [1, 2, 3, 4];
    let sink = 0;
    for (let i = 0; i < 40000; ++i) // warm up: every function reaches the FTL (or the DFG)
        sink += newDynamic(4).length + sliceFour(source).length + mapFour(four).length;
    const before = $vm.jsThreadsCounter("newArrayWithSizeOperation");
    const CALLS = 100000;
    for (let i = 0; i < CALLS; ++i)
        sink += newDynamic(4).length + sliceFour(source).length + mapFour(four).length;
    const took = $vm.jsThreadsCounter("newArrayWithSizeOperation") - before;
    if (sink !== (40000 + CALLS) * 12)
        throw new Error("wrong lengths");
    return took;
}

const onMain = exercise();
if (onMain > 180000)
    throw new Error("main thread: " + onMain + " of 300000 variable-size array allocations called the generic operation");
const onSpawned = new Thread(exercise).join();
if (onSpawned > 180000)
    throw new Error("spawned thread: " + onSpawned + " of 300000 variable-size array allocations called the generic operation");
