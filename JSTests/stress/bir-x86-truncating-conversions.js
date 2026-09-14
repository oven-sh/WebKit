//@ skip if !$isFTLPlatform
//@ skip if $architecture != "x86_64"
//@ requireOptions("--useDollarVM=1")

// The two vector conversions that are what x86-64's own instructions do (cvttps2dq, cvttpd2dq: `_mm_cvttps_epi32`,
// `_mm_cvttpd_epi32`): a lane that does not fit, or is not a number, becomes 0x80000000. The portable ones next
// to them saturate, and turn NaN into 0.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const b = n => ({ u8: n });
const s = n => ({ s: n });
const KIND = { F32x4ToI32x4S: 2, F64x2ToI32x4ZeroS: 6, F32x4ToI32x4X86: 26, F64x2ToI32x4ZeroX86: 27 };
// void convert(int out[4], const void* in) { *(v4si*)out = convert(*(vector*)in); }
const funcs = [], exports = [];
for (const [name, kind] of Object.entries(KIND)) {
    exports.push({ name, func: funcs.length, ret: FFI.void, args: [FFI.ptr, FFI.ptr] });
    funcs.push({ name, sig: 0, exported: true, blocks: [[["Load", b(MEM.v128), 1, s(0)], ["VConvert", b(kind), 2], ["Store", b(MEM.v128), 3, 0, s(0)], ["RetVoid"]]] });
}
const m = $vm.cModule(assemble({ sigs: [{ ret: T.void, params: [T.i64, T.i64] }], funcs, exports }));

const INDEFINITE = -0x80000000;
const out = new Int32Array(4);
function check(name, input, expected) {
    out.fill(7);
    m[name](out, input);
    for (let lane = 0; lane < expected.length; lane++)
        eq(out[lane], expected[lane], `${name}(${Array.from(input)}) lane ${lane}`);
}
const floats = [[3e9, NaN, -2.5, 1.5], [Infinity, -Infinity, 2147483648, -2147483648], [2147483520, -2147483904, 0.9999999, -0.9999999], [-3e9, 1e30, -0, 100.99]];
const x86Floats = [[INDEFINITE, INDEFINITE, -2, 1], [INDEFINITE, INDEFINITE, INDEFINITE, INDEFINITE], [2147483520, INDEFINITE, 0, 0], [INDEFINITE, INDEFINITE, 0, 100]];
const saturatedFloats = [[2147483647, 0, -2, 1], [2147483647, -2147483648, 2147483647, -2147483648], [2147483520, -2147483648, 0, 0], [-2147483648, 2147483647, 0, 100]];
for (let i = 0; i < floats.length; i++) {
    check("F32x4ToI32x4X86", new Float32Array(floats[i]), x86Floats[i]);
    check("F32x4ToI32x4S", new Float32Array(floats[i]), saturatedFloats[i]);
}
const doubles = [[3e9, NaN], [-2.5, 1.5], [Infinity, -Infinity], [2147483647.9, -2147483648.9], [2147483648, -2147483649], [-0, 1e300]];
const x86Doubles = [[INDEFINITE, INDEFINITE], [-2, 1], [INDEFINITE, INDEFINITE], [2147483647, -2147483648], [INDEFINITE, INDEFINITE], [0, INDEFINITE]];
const saturatedDoubles = [[2147483647, 0], [-2, 1], [2147483647, -2147483648], [2147483647, -2147483648], [2147483647, -2147483648], [0, 2147483647]];
for (let i = 0; i < doubles.length; i++) {
    check("F64x2ToI32x4ZeroX86", new Float64Array(doubles[i]), [...x86Doubles[i], 0, 0]);
    check("F64x2ToI32x4ZeroS", new Float64Array(doubles[i]), [...saturatedDoubles[i], 0, 0]);
}
print("x86 truncating conversions ok");
