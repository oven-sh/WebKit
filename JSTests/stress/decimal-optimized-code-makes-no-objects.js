//@ requireOptions("--useDecimal=1", "--useConcurrentJIT=0")
//@ runDefault
// Only in this mode: in others the code may not have reached the FTL. No result tells whether the compiler knew the function that is
// called, but the number of objects does: one in each turn takes 32 bytes. The heap counts in blocks, of up to 64 KB.

function test(what, body) {
    const run = Function("x", "one", "n", body);
    noInline(run);
    const [x, one] = [new Decimal("19.99"), new Decimal(1)];
    for (let i = 0; i < testLoopCount / 5; ++i)
        run(x, one, 50);
    const turns = 20000;
    const before = $vm.heapTotalBytesAllocated();
    run(x, one, turns);
    const bytes = $vm.heapTotalBytesAllocated() - before;
    if ($vm.useFTLJIT() && bytes >= 8 * turns)
        throw new Error(`${what}: ${bytes} bytes in ${turns} turns`);
}

for (const call of ["abs()", "negate()", "add(x)", "subtract(x)", "multiply(one)", "divide(one)", "remainder(x)", "round(2)", "scale10(0)", "mantissa()"])
    test(call, `let total = x; for (let i = 0; i < n; ++i) total = total.${call}; return total.isNaN();`);

for (const call of ["isNaN()", "isFinite()", "exponent()", "compare(x)", "equals(x)", "notEquals(x)", "lessThan(x)", "lessThanOrEqual(x)", "greaterThan(x)",
    "greaterThanOrEqual(x)", "toString()", "toExponential()", "toFixed({ digits: 1 })"])
    test(call, `let value = x, count = 0; for (let i = 0; i < n; ++i) { value = value.negate(); if (value.${call}) ++count; } return count;`);
