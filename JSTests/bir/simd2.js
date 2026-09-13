load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
const module = assemble({
    sigs: [{ ret: T.i64, params: [T.i64, T.i64] }, { ret: T.void, params: [T.i64, T.i64, T.i64] }],
    funcs: [
        { name: "umulh", sig: 0, exported: true, blocks: [[["UMulHigh", 0, 1], ["Ret", 2]]] },
        { name: "mulh", sig: 0, exported: true, blocks: [[["MulHigh", 0, 1], ["Ret", 2]]] },
        // out[0] = addsat_u8(a,b); out[1] = subsat_s16(a,b); out[2] = avg_u8(a,b); out[3] = extmul_high_s16->i32(a,b); out[4] = narrow_s32->i16 (a,b);
        // out[5] = dot(a,b); out[6] = swizzle(a, b); out[7] = i64x2->f64x2 signed (a); out[8] = f64x2 -> i64x2 (of out[7])
        { name: "ops", sig: 1, exported: true, blocks: [[
            ["Load", t8(MEM.v128), 1, s(0)], ["Load", t8(MEM.v128), 2, s(0)],
            ["VAddSat", t8(LANE.i8x16), t8(0), 3, 4], ["Store", t8(MEM.v128), 5, 0, s(0)],
            ["VSubSat", t8(LANE.i16x8), t8(1), 3, 4], ["Store", t8(MEM.v128), 6, 0, s(16)],
            ["VAvgU", t8(LANE.i8x16), 3, 4], ["Store", t8(MEM.v128), 7, 0, s(32)],
            ["VExtMul", t8(LANE.i32x4), t8(1), t8(1), 3, 4], ["Store", t8(MEM.v128), 8, 0, s(48)],
            ["VNarrow", t8(LANE.i32x4), t8(1), 3, 4], ["Store", t8(MEM.v128), 9, 0, s(64)],
            ["VDot", 3, 4], ["Store", t8(MEM.v128), 10, 0, s(80)],
            ["VSwizzle", 3, 4], ["Store", t8(MEM.v128), 11, 0, s(96)],
            ["VConvert", t8(22), 3], ["Store", t8(MEM.v128), 12, 0, s(112)],
            ["VConvert", t8(24), 12], ["Store", t8(MEM.v128), 13, 0, s(128)],
            ["RetVoid"],
        ]] },
    ],
    exports: [
        { name: "umulh", func: 0, ret: FFI.u64, args: [FFI.u64, FFI.u64] },
        { name: "mulh", func: 1, ret: FFI.i64, args: [FFI.i64, FFI.i64] },
        { name: "ops", func: 2, ret: FFI.void, args: [FFI.ptr, FFI.ptr, FFI.ptr] },
    ],
});
const m = $vm.cModule(module);
eq(m.umulh(0xffffffffffffffffn, 0xffffffffffffffffn), 0xfffffffffffffffen, "umulh");
eq(m.mulh(-1n, 5n), -1n, "mulh(-1, 5)");
eq(m.umulh(1n << 63n, 4n), 2n, "umulh(2^63, 4)");

const out = new ArrayBuffer(144), a = new ArrayBuffer(16), b = new ArrayBuffer(16);
const u8 = (buf, off = 0) => new Uint8Array(buf, off, 16), i16 = (buf, off = 0) => new Int16Array(buf, off, 8), i32 = (buf, off = 0) => new Int32Array(buf, off, 4);
// bytes of a: 250,5,100,200, then 0..; of b: 10,10,3,100, then indices for swizzle use all of b
u8(a).set([250, 5, 100, 200, 1, 2, 3, 4, 9, 0, 0, 0, 0, 0, 0x80, 0x7f]);
u8(b).set([10, 10, 3, 100, 0, 15, 16, 255, 1, 1, 1, 1, 2, 2, 2, 2]);
m.ops(out, a, b);
eq(Array.from(u8(out, 0).subarray(0, 4)).join(), "255,15,103,255", "u8 saturating add");
eq(i16(out, 16)[0], i16(a)[0] - i16(b)[0] < -32768 ? -32768 : i16(a)[0] - i16(b)[0], "s16 saturating sub lane 0");
eq(Array.from(u8(out, 32).subarray(0, 4)).join(), [130, 8, 52, 150].join(), "u8 rounding average");
eq(i32(out, 48)[0], i16(a)[4] * i16(b)[4], "extmul high half lane 0 (s16*s16 -> s32)");
{ const sat = v => Math.max(-32768, Math.min(32767, v)); eq(i16(out, 64)[0], sat(i32(a)[0]), "narrow a lane 0"); eq(i16(out, 64)[4], sat(i32(b)[0]), "narrow b lane 0"); }
eq(i32(out, 80)[0], i16(a)[0] * i16(b)[0] + i16(a)[1] * i16(b)[1], "dot lane 0");
{ const idx = u8(b), src = u8(a), got = u8(out, 96); for (let i = 0; i < 16; i++) eq(got[i], idx[i] < 16 ? src[idx[i]] : 0, "swizzle byte " + i); }
{ const big = new BigInt64Array(a), f = new Float64Array(out, 112, 2), back = new BigInt64Array(out, 128, 2); eq(f[0], Number(big[0]), "i64 -> f64 lane 0"); eq(f[1], Number(big[1]), "i64 -> f64 lane 1"); eq(back[0], BigInt(Math.trunc(f[0])), "f64 -> i64"); }
print("simd2 ok");
