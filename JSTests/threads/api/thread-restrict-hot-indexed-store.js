//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ runDefault()
//@ runDefault("--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=50", "--thresholdForOptimizeSoon=50", "--thresholdForFTLOptimizeAfterWarmUp=300", "--thresholdForFTLOptimizeSoon=300")
// API-I14 (api §5.8) against optimized code, tenth round (SPEC-objectmodel G1).
// A restricted array is pinned on an uncacheable-dictionary SlowPutArrayStorage
// shape so that every indexed store reaches the generic entry points, which
// check the restriction. Two fast paths store an existing in-vector element of
// a SlowPut array without passing them: JSObject::trySetIndexQuickly's SlowPut
// arm and the DFG/FTL PutByVal for a SlowPutArrayStorage array mode. With
// per-thread butterfly tags every store from a thread other than the array's
// allocator failed the write predicate and went to the slow path; with the GIL
// on the words are untagged now, so both are closed whenever the flag is on.
// The store site here is made hot on restricted arrays its own thread owns
// (allowed: the owner may write), in every tier, and is then handed an array
// another thread restricted: the store must throw ConcurrentAccessError and
// leave the element alone.
load("../resources/assert.js", "caller relative");

function store(a, i, v) { a[i] = v; }
noInline(store);

const foreignOwned = Thread.restrict([1, 2, 3, 4]); // owned by the main thread
const verdict = new Thread(() => {
    // Arrays this thread owns and restricts: same shape as foreignOwned, stores allowed.
    const mine = [];
    for (let k = 0; k < 8; ++k)
        mine.push(Thread.restrict([1, 2, 3, 4]));
    for (let n = 0; n < 20000; ++n) {
        const a = mine[n & 7];
        store(a, n & 3, n);
        if (a[n & 3] !== n)
            return "the owner's own store was lost at " + n;
    }
    let outcome = "no exception";
    try {
        store(foreignOwned, 0, 99);
    } catch (e) {
        outcome = e instanceof ConcurrentAccessError ? "CAE" : ("threw " + e);
    }
    return outcome;
}).join();
shouldBe(verdict, "CAE", "a hot indexed store to an array another thread restricted");
shouldBe(foreignOwned[0], 1, "the restricted array's element");
foreignOwned[0] = 5; // the owner still may
shouldBe(foreignOwned[0], 5);
