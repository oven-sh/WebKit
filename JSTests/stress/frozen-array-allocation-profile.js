//@ runDefault("--useConcurrentJIT=false", "--useDFGJIT=false", "--collectContinuously=true", "--useGenerationalGC=false", "--thresholdForJITAfterWarmUp=10")

// Object.freeze switches an array to the SlowPutArrayStorage shape before it transitions the
// structure. A collection that ends inside that window updates the allocation profile of the site
// that created the array; the site must not start allocating SlowPutArrayStorage arrays.

function build(n) { let a = []; for (let i = 0; i < n; ++i) a.push(i); return a; }
noInline(build);

for (let i = 0; i < 20000; ++i) {
    let frozen = build(8);
    Object.freeze(frozen);
    let next = build(8);
    if ($vm.indexingMode(next) === "ArrayWithSlowPutArrayStorage" && !$vm.isHavingABadTime(next))
        throw new Error("iteration " + i + ": the site allocates SlowPutArrayStorage arrays");
    next[0] = 1;
    next.push(9);
}
