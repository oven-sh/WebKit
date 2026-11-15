// A read, by name, of a global that is an accessor. The DFG and the FTL cannot prove where such a property is,
// so they read it through an inline cache.
Object.defineProperty(globalThis, "accessorGlobal", { get() { return 3; }, configurable: true });

function test(count)
{
    let sum = 0;
    for (let i = 0; i < count; ++i)
        sum += accessorGlobal;
    return sum;
}
noInline(test);

for (let i = 0; i < 500; ++i) {
    if (test(1e4) !== 3e4)
        throw new Error("bad result");
}
