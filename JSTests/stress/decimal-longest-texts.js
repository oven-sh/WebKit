//@ skip if $memoryLimited
//@ requireOptions("--useDecimal=1")
//@ runDefault
// Near the greatest count of digits the bound of the length does not fit in 32 bits. One string of 2 GB at a time.
for (const f of [() => new Decimal("1e-6176").toFixed({ digits: 2147477447 }).length, () => new Decimal("1e-6176").toPrecision({ precision: 2147477447 }).length]) {
    const length = f();
    if (length < 2147477447)
        throw new Error("length " + length);
    fullGC();
}
