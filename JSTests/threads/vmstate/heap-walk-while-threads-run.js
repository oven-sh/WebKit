//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=50")
// Heap walks and code deletion while other threads allocate and run JIT code.
//
// A walk of the block directories (HeapIterationScope, Heap::size(),
// Heap::objectCount()) flushes or reads state that another client's
// allocation fast path owns, so with the GIL off it must stop the other
// threads or exclude their slow paths. deleteAllCode must in addition wait
// until no thread is inside JS: a thread that is stopped inside JS would
// return into deleted code. This test runs both from the main thread and
// from spawned threads while the others allocate, then checks that the
// JIT-compiled function and the regexps still compute the right values.
//
// Workers run a fixed number of iterations and never spin on shared state,
// so the test also completes with the GIL on, where threads yield only at
// blocking calls.
load("../harness.js", "caller relative");

const WORKERS = 4;
const ROUNDS = 300;

function hot(o, i) {
    return o.a + o.b * i;
}

function work(tid) {
    let sum = 0;
    let walks = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        const objects = [];
        for (let i = 0; i < 40; ++i)
            objects.push({ a: i, b: tid, c: [i, r], d: "s" + i });
        for (let i = 0; i < objects.length; ++i)
            sum += hot(objects[i], 2);
        if (!/x(\d+)y/.test("ax" + r + "yb"))
            throw new Error("regexp failed in thread " + tid + " round " + r);
        if ((r % 25) === tid) {
            walks += $vm.globalObjectCount() > 0 ? 1 : 0;
            walks += gcHeapSize() >= 0 ? 1 : 0;
        }
        if ((r % 97) === tid)
            $vm.deleteAllCodeWhenIdle();
    }
    return { sum, walks };
}

function expectedSum(tid) {
    let sum = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        for (let i = 0; i < 40; ++i)
            sum += i + tid * 2;
    }
    return sum;
}

const threads = spawnN(WORKERS, work);

for (let k = 0; k < 40; ++k) {
    shouldBeTrue($vm.globalObjectCount() >= 1, "main thread globalObjectCount");
    shouldBeTrue(gcHeapSize() >= 0, "main thread heap size");
    if (k % 8 === 0)
        $vm.deleteAllCodeWhenIdle();
    sleepMs(1);
}

const results = joinAll(threads);
for (let t = 0; t < WORKERS; ++t) {
    shouldBe(results[t].sum, expectedSum(t), "sum computed by thread " + t);
    shouldBeTrue(results[t].walks > 0, "thread " + t + " walked the heap");
}

// The main thread's own code after the deletions above.
let mainSum = 0;
for (let i = 0; i < 1000; ++i)
    mainSum += hot({ a: 1, b: 2 }, i);
shouldBe(mainSum, 1000 + 2 * (999 * 1000 / 2), "main thread sum after deleteAllCode");
