//@ requireOptions("--useDecimal=1", "--useDecimalLiterals=1", "--useDecimalOperators=1", "--useConcurrentJIT=0")
//@ runDefault
// As decimal-optimized-code-makes-no-objects.js.

for (const operand of ["0.01m", "-0.01m", "x + x", "x * 2m", "-x"]) {
    const run = Function("x", "n", `let total = x; for (let i = 0; i < n; ++i) total = total.add(${operand}); return total.isNaN();`);
    noInline(run);
    const x = 19.99m;
    for (let i = 0; i < testLoopCount / 5; ++i)
        run(x, 50);
    const turns = 20000;
    const before = $vm.heapTotalBytesAllocated();
    run(x, turns);
    const bytes = $vm.heapTotalBytesAllocated() - before;
    if ($vm.useFTLJIT() && bytes >= 8 * turns)
        throw new Error(`${operand}: ${bytes} bytes in ${turns} turns`);
}
