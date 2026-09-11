//@ runDefault("--useConcurrentJIT=false")
//@ runDefault("--useConcurrentJIT=false", "--useLazyValueProfilePredictions=false")
//@ runDefault("--useConcurrentJIT=false", "--thresholdForValueProfilePredictions=1000000")
//@ runDefault("--useConcurrentJIT=false", "--collectContinuously=true", "--useGenerationalGC=false")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

// A function runs twice, collections come and go while its samples are still in the buckets, and then it gets hot.
// The optimizing compilers must find the types it kept seeing, whatever happened to the first samples.
function make(i) { return { x: i, y: { z: i + 0.5 }, s: "s" + i, a: [i, i + 1] }; }
function work(o, k) {
    let t = o.y;
    let u = t.z + o.x;
    let v = o.s.length + o.a[k];
    let w = { u, v };
    return w.u + w.v;
}
noInline(work);

let expected = (i) => i + 0.5 + i + ("s" + i).length + i + 1;
shouldBe(work(make(1), 1), expected(1));
fullGC();
shouldBe(work(make(2), 1), expected(2));
edenGC();
fullGC();
for (let i = 3; i < 30; ++i) {
    shouldBe(work(make(i), 1), expected(i));
    if (i % 7 === 0)
        edenGC();
}
fullGC();
for (let i = 0; i < 20000; ++i)
    shouldBe(work(make(i), 1), expected(i));

// Every executed op of work() had something to say by the time it was compiled: no exits, one compile.
if ($vm.useDFGJIT() && !jscOptions().collectContinuously) {
    shouldBe(numberOfDFGCompiles(work) <= 1, true);
    shouldBe(reoptimizationRetryCount(work), 0);
}

// The same when the first samples were objects that died.
function dying(n) {
    let o = { p: n, q: [n] };
    let r = o.q;
    return r[0] + o.p;
}
noInline(dying);
shouldBe(dying(1), 2);
shouldBe(dying(2), 4);
fullGC();
edenGC();
for (let i = 0; i < 20000; ++i)
    shouldBe(dying(i), 2 * i);
if ($vm.useDFGJIT() && !jscOptions().collectContinuously) {
    shouldBe(numberOfDFGCompiles(dying) <= 1, true);
    shouldBe(reoptimizationRetryCount(dying), 0);
}
