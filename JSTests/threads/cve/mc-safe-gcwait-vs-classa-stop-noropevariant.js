//@ requireOptions("--useJSThreads=1", "--useThreadGILOffUnsafe=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100")
// MC-SAFE S4 mechanism-only variant: identical to mc-safe-gcwait-vs-classa-stop.js
// but with all main-thread JSRopeString construction removed, so the S4
// GCL-ordering-shield mechanism can be exercised independently of the
// JSRopeString length-sum assertion that the original test currently hits
// 5/5 under a concurrent gc() storm (see mc-safe-gcwait-rope-repro.js for
// the isolated side-finding). Per-round CodeBlock freshness is obtained via
// distinct closure identity instead of distinct source text.
load("../harness.js", "caller relative");

const GC_THREADS = 2;
const ROUNDS = 12;
const gate = { started: 0, stop: 0 };

const gcers = spawnN(GC_THREADS, () => {
    Atomics.add(gate, "started", 1);
    let cycles = 0;
    let churn = null;
    while (Atomics.load(gate, "stop") === 0) {
        churn = new Array(4096).fill(cycles);
        gc();
        ++cycles;
    }
    return cycles + (churn ? 1 : 0);
});

waitUntil(() => Atomics.load(gate, "started") === GC_THREADS);

const nowMs = (typeof preciseTime === "function") ? () => preciseTime() * 1000 : () => Date.now();

function buildVictim() {
    const proto = { y: 1 };
    const o = Object.create(proto);
    // No string concat: fresh closure per call so each round gets its own
    // FunctionExecutable / CodeBlock and its own un-fired replacement
    // watchpoint.
    const f = function (o) { return o.y + 1; };
    for (let i = 0; i < 2000; ++i)
        f(o);
    return { proto, o, f };
}

let slowestMs = 0;
for (let r = 0; r < ROUNDS; ++r) {
    const v = buildVictim();
    const t0 = nowMs();
    v.proto.y = 2 + r; // Class-A fire => jettison => §A.3 stop, racing the GC storm.
    const ms = nowMs() - t0;
    if (ms > slowestMs)
        slowestMs = ms;
    shouldBe(v.f(v.o), 3 + r);
    if (!(ms < 20000))
        throw new Error("S4 round did not converge under 20s");
}

Atomics.store(gate, "stop", 1);
const counts = joinAll(gcers);
for (const c of counts)
    if (!(c > 0))
        throw new Error("GC thread made no progress");
