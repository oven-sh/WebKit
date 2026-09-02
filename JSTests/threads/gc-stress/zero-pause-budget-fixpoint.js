//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--minimumGCPauseMS=0", "--gcPauseScale=0", "--maximumMutatorUtilization=0.01")
// With a zero pause budget, the time at which the mutator should resume has
// always passed, so each marking drain stops at once. Upstream then resumes the
// mutator, and the markers make progress. In the shared heap, the fixpoint
// often does not resume, and it looped with the same past deadline: the
// collection never finished.
//
// The weak references and the WeakMap give the fixpoint work to drain after
// its first pass; without them, the first pass finishes the marking.
const keep = [];
const weak = [];
const map = new WeakMap();
for (let i = 0; i < 2000; ++i) {
    const cell = new Array(50).fill(i + 0.5);
    keep.push([cell, 0]);
    weak.push(new WeakRef(cell));
    map.set(cell, { i });
}
for (let round = 0; round < 3; ++round)
    $vm.gc();
for (let i = 0; i < 2000; ++i) {
    if (weak[i].deref() !== keep[i][0] || map.get(keep[i][0]).i !== i)
        throw new Error("bad entry " + i);
}
