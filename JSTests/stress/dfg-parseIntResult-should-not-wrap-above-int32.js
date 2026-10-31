// parseInt results outside int32 range must be boxed as doubles, not
// sign-wrapped int32s, once the call site tiers up to the DFG/FTL.
// parseIntResult() used to do static_cast<int>(input) before the round-trip
// comparison deciding int32 vs double boxing; the cast is undefined behavior
// for out-of-range inputs, which let LTO fold the overflow guard away.

function hex(s) {
    return parseInt(s, 16);
}
noInline(hex);

function dec(s) {
    return parseInt(s);
}
noInline(dec);

const hugeHex = "f".repeat(400);

for (let i = 0; i < testLoopCount; ++i) {
    let v = hex("80000000");
    if (v !== 2147483648)
        throw "FAILED: parseInt('80000000', 16) === " + v + " at iteration " + i;
    v = hex("ffffffff");
    if (v !== 4294967295)
        throw "FAILED: parseInt('ffffffff', 16) === " + v + " at iteration " + i;
    v = hex("7fffffff");
    if (v !== 2147483647)
        throw "FAILED: parseInt('7fffffff', 16) === " + v + " at iteration " + i;
    v = hex("-80000001");
    if (v !== -2147483649)
        throw "FAILED: parseInt('-80000001', 16) === " + v + " at iteration " + i;
    v = dec("2147483648");
    if (v !== 2147483648)
        throw "FAILED: parseInt('2147483648') === " + v + " at iteration " + i;
    v = hex(hugeHex); // overflows to Infinity
    if (v !== Infinity)
        throw "FAILED: parseInt('f'.repeat(400), 16) === " + v + " at iteration " + i;
}
