//@ requireOptions("--useJSThreads=1", "--useThreadedDFG=0", "--useDollarVM=1")
// useThreadedDFG=0 is a tier kill switch under useJSThreads: option
// canonicalization must turn useDFGJIT (and with it useFTLJIT) off, and no
// function may ever run in optimized code. The baseline JIT and the LLInt
// stay on; the results must not change.

function check(cond, msg) { if (!cond) throw new Error(msg); }

check($vm.useDFGJIT() === false, "useThreadedDFG=0 must disable useDFGJIT under useJSThreads");
check($vm.useFTLJIT() === false, "useThreadedDFG=0 must disable useFTLJIT under useJSThreads");
check($vm.useJIT() === true, "the kill switch must not disable the JIT as a whole");

function hot(o, i) {
    if ($vm.dfgTrue())
        throw new Error("optimized code ran with useThreadedDFG=0 at iteration " + i);
    return o.a + o.b + i;
}
noInline(hot);

var o = { a: 1, b: 2 };
var sum = 0;
for (var i = 0; i < 300000; ++i)
    sum += hot(o, i);
check(sum === 300000 * 3 + (300000 * 299999) / 2, "sum mismatch: " + sum);
