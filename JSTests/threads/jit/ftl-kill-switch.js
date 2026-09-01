//@ requireOptions("--useJSThreads=1", "--useThreadedFTL=0", "--useDollarVM=1")
// useThreadedFTL=0 is a tier kill switch under useJSThreads: option
// canonicalization must turn useFTLJIT off, and no function may ever run in
// FTL code. The DFG and the lower tiers stay on; the results must not change.

function check(cond, msg) { if (!cond) throw new Error(msg); }

check($vm.useFTLJIT() === false, "useThreadedFTL=0 must disable useFTLJIT under useJSThreads");
check($vm.useJIT() === true, "the kill switch must not disable the JIT as a whole");
check($vm.useDFGJIT() === true, "the kill switch must not disable the DFG");

function hot(o, i) {
    if ($vm.ftlTrue())
        throw new Error("FTL code ran with useThreadedFTL=0 at iteration " + i);
    return o.a + o.b + i;
}
noInline(hot);

var o = { a: 1, b: 2 };
var sum = 0;
for (var i = 0; i < 300000; ++i)
    sum += hot(o, i);
check(sum === 300000 * 3 + (300000 * 299999) / 2, "sum mismatch: " + sum);
