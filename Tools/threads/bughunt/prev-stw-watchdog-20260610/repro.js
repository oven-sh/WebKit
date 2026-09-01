// repro.js — focused distillation of the STW-watchdog deadlock
// (stw-watchdog evidence pack, 2026-06-10).
//
// Mechanism under test (from core.585068 / core.593320 backtraces):
//   - Thread X in Baseline put-IC slow path: tryCachePutBy takes
//     GCSafeConcurrentJSLocker(codeBlock->m_lock), ExistingProperty branch
//     calls Structure::didCachePropertyReplacement (Repatch.cpp:1360)
//     -> Class-A WatchpointSet fire -> stopTheWorldAndRun WHILE HOLDING
//     codeBlock->m_lock.
//   - Sibling threads in the same hot function's get/put IC slow paths block
//     on the SAME codeBlock->m_lock holding heap access (no stop poll inside
//     WTF::Lock::lock) -> the SectionA.3.2 conductor predicate never
//     converges -> 30s watchdog SIGABRT.
//
// So the repro needs: N>=2 threads executing ONE hot function whose body does
// existing-property replace puts (forces the didCachePropertyReplacement
// fire during IC warmup) plus property gets (sibling blockers), on
// identically-shaped thread-local objects (same Structure, same CodeBlock).
//
// Run (deadlocks intermittently; ~30s SIGABRT when it does):
//   jsc --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 \
//       --useSharedAtomStringTable=1 --useSharedGCHeap=1 \
//       --useThreadGILOffUnsafe=1 repro.js
//
// N configurable: -e "globalThis.REPRO_THREADS=2;"

function work() {
    function Obj() {
        this.link = null;
        this.id = 0;
        this.kind = 0;
        this.a1 = 0;
        this.hops = 0;
        this.scratch = 0;
    }
    const objs = [];
    for (let i = 0; i < 8; ++i) {
        const o = new Obj();
        o.id = i;
        objs.push(o);
    }
    let acc = 0;
    for (let s = 0; s < 2000000; ++s) {
        const o = objs[s & 7];
        // Existing-property replace puts: each distinct property offset has
        // its own replacement WatchpointSet; the first cached replace per
        // (structure, offset) fires Class-A under codeBlock->m_lock.
        o.scratch = (o.scratch + s) & 0xffffff;
        o.a1 = (o.a1 + 1) | 0;
        o.hops = (o.hops + o.id) & 0xffff;
        o.kind = s & 3;
        // Gets keep sibling threads queued on the same codeBlock lock.
        acc = (acc + o.a1 + o.hops + o.scratch + o.kind) | 0;
    }
    return "" + acc;
}

const n = (typeof globalThis.REPRO_THREADS === "number") ? globalThis.REPRO_THREADS : 4;

if (typeof Thread !== "function")
    throw new Error("requires --useJSThreads=1");

const reference = work();
const threads = [];
for (let i = 0; i < n; ++i)
    threads.push(new Thread(work, i));
const results = threads.map(t => t.join());
for (let i = 0; i < n; ++i) {
    if (results[i] !== reference)
        throw new Error("checksum divergence: thread " + i + " got " + results[i] + " want " + reference);
}
print("REPRO-OK n=" + n);
