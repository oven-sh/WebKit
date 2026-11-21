// A read, by name, of a property that the global object inherits. The DFG and the FTL cannot prove where such
// a property is, so they read it through an inline cache.
Object.prototype.inheritedGlobal = 3;

function test(count)
{
    let sum = 0;
    for (let i = 0; i < count; ++i)
        sum += inheritedGlobal;
    return sum;
}
noInline(test);

for (let i = 0; i < 500; ++i) {
    if (test(1e4) !== 3e4)
        throw new Error("bad result");
}
