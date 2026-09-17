//@ requireOptions("--useJSThreads=1", "--useConcurrentJIT=0")
// SPEC-jit §5.8, history §58 (tenth round). GIL off a polymorphic call stub's slot
// cannot be rewritten in place when that variant's callee tiers up (other threads
// execute through the slots), and until the tenth round the fallback was to unlink
// the whole site: every tier-up of every variant reset the site to "monomorphic
// on whoever comes next", variant list and call counts gone. The DFG then
// speculated on that one callee, exited when the next one came, and the caller
// was recompiled again and again, each time seeing one callee. JetStream Basic:
// 13,205 exits GIL off against 5,890 with the GIL on, 4,002 of them at the
// callback call inside Array.prototype.map. Now the tier-up publishes a copy of
// the stub with that slot upgraded and the counts kept, and the site behaves as
// on main.
// Here: map() over four closures in turn, in stretches of 512 calls, each closure
// tiering up along the way. With the synchronous JIT the GIL-on (and main) numbers
// are 703 BadConstantValue exits and 4 DFG compilations of map; GIL off it was
// 3,105 and 6, and is 703 and 4 now.
// (--useConcurrentJIT=0: the count is taken right after the loop; GIL off that is
// the parked wait of history §56.)
load("../harness.js", "caller relative");

function run(arr, k) {
    switch (k) {
    case 0: return arr.map(x => x + 1);
    case 1: return arr.map(x => x * 2);
    case 2: return arr.map(x => x - 3);
    default: return arr.map(x => x ^ 5);
    }
}
noInline(run);

const arr = [1, 2, 3, 4, 5, 6, 7, 8];
const expected = [5, 8, 1, 1];
for (let i = 0; i < 300000; ++i) {
    const k = (i >> 9) & 3;
    if (run(arr, k)[3] !== expected[k])
        throw new Error("wrong element for callback " + k + " at iteration " + i);
}

const compiles = numberOfDFGCompiles(Array.prototype.map);
if (!(compiles >= 1 && compiles <= 4))
    throw new Error("Array.prototype.map was compiled by the DFG " + compiles + " times");
