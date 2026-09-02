//@ requireOptions("--useConcurrentJIT=0")
//@ runDefault()
//@ runDefault("--useJSThreads=1")
// A for-of loop whose body captures a const binding copies the binding into a
// new scope on each iteration. On the first iteration of each call the value
// copied is the TDZ sentinel, which the value profile never records, so the
// fixup phase used to insert a check that exits on every call and is inserted
// again on every recompile. The first run has threads off: the fix must not
// depend on them. The tier-up thresholds are the defaults, as in
// JSTests/stress/for-of-const-closure-captured-tier-up.js: lower ones add an
// unrelated recompile.

function hot(arr) {
    let sum = 0;
    for (const x of arr)
        (() => sum += x)();
    return sum;
}
noInline(hot);

const a = new Int32Array(10).map((_, i) => i);
for (let i = 0; i < 3000; ++i) {
    if (hot(a) !== 45)
        throw new Error("bad sum");
}

// One recompile is allowed: the first compile can take the exit before the
// exit site is recorded.
if (numberOfDFGCompiles(hot) > 2)
    throw new Error("hot() was compiled " + numberOfDFGCompiles(hot) + " times");
