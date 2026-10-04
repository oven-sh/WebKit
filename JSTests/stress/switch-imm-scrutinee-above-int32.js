// A double scrutinee outside int32 range must never match an immediate
// switch case. slow_path_switch_imm used to do static_cast<int32_t>(value)
// — undefined behavior for out-of-range doubles — before the range check;
// a UB-exploiting optimizer could then fold the check away and match the
// case whose label equals the wrapped/saturated conversion result.

function bigSwitch(x) {
    switch (x) {
    case -2147483648:
        return "int32-min";
    case 2147483647:
        return "int32-max";
    case 0:
        return "zero";
    default:
        return "default";
    }
}
noInline(bigSwitch);

function denseSwitch(x) {
    // Dense enough for a jump table (SwitchType::Immediate).
    switch (x) {
    case -2147483648: return 0;
    case -2147483647: return 1;
    case -2147483646: return 2;
    case -2147483645: return 3;
    case -2147483644: return 4;
    case -2147483643: return 5;
    case -2147483642: return 6;
    case -2147483641: return 7;
    default: return -1;
    }
}
noInline(denseSwitch);

for (let i = 0; i < testLoopCount; ++i) {
    let r = bigSwitch(2147483648); // 2^31, truncates to INT32_MIN on x86
    if (r !== "default")
        throw "FAILED: bigSwitch(2^31) === " + r + " at iteration " + i;
    r = bigSwitch(4294967296); // 2^32, truncates to 0 via modular wrap
    if (r !== "default")
        throw "FAILED: bigSwitch(2^32) === " + r + " at iteration " + i;
    r = bigSwitch(Infinity);
    if (r !== "default")
        throw "FAILED: bigSwitch(Infinity) === " + r + " at iteration " + i;
    r = bigSwitch(-2147483648);
    if (r !== "int32-min")
        throw "FAILED: bigSwitch(-2^31) === " + r + " at iteration " + i;
    r = denseSwitch(2147483648);
    if (r !== -1)
        throw "FAILED: denseSwitch(2^31) === " + r + " at iteration " + i;
    r = denseSwitch(-2147483648);
    if (r !== 0)
        throw "FAILED: denseSwitch(-2^31) === " + r + " at iteration " + i;
}
