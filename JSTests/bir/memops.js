load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const sizes = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 24, 31, 33, 63, 64, 65, 100];
const funcs = [], exports = [];
for (const n of sizes) {
    funcs.push({ name: "copy" + n, sig: 0, exported: true, blocks: [[["ConstI64", s(n)], ["MemCopy", 0, 1, 2], ["RetVoid"]]] });
    exports.push({ name: "copy" + n, func: funcs.length - 1, ret: FFI.void, args: [FFI.ptr, FFI.ptr] });
    funcs.push({ name: "fill" + n, sig: 1, exported: true, blocks: [[["ConstI64", s(n)], ["MemSet", 0, 1, 2], ["RetVoid"]]] });
    exports.push({ name: "fill" + n, func: funcs.length - 1, ret: FFI.void, args: [FFI.ptr, FFI.i32] });
}
const m = $vm.cModule(assemble({ sigs: [{ ret: T.void, params: [T.i64, T.i64] }, { ret: T.void, params: [T.i64, T.i32] }], funcs, exports }));
for (const n of sizes) {
    // separate buffers
    const src = new Uint8Array(128).map((_, i) => (i * 37 + 11) & 255), dst = new Uint8Array(128).fill(0xee);
    m["copy" + n](dst.subarray(8), src.subarray(3));
    for (let i = 0; i < 128; i++) eq(dst[i], i >= 8 && i < 8 + n ? src[i - 8 + 3] : 0xee, `copy${n} byte ${i}`);
    // overlapping, both directions (memmove semantics)
    for (const shift of [1, 3, 8]) {
        const a = new Uint8Array(200).map((_, i) => i), expectFwd = a.slice(); expectFwd.copyWithin(10 + shift, 10, 10 + n);
        m["copy" + n](a.subarray(10 + shift), a.subarray(10));
        for (let i = 0; i < 200; i++) eq(a[i], expectFwd[i], `overlap forward copy${n} shift ${shift} byte ${i}`);
        const b = new Uint8Array(200).map((_, i) => i), expectBack = b.slice(); expectBack.copyWithin(10, 10 + shift, 10 + shift + n);
        m["copy" + n](b.subarray(10), b.subarray(10 + shift));
        for (let i = 0; i < 200; i++) eq(b[i], expectBack[i], `overlap backward copy${n} shift ${shift} byte ${i}`);
    }
    const f = new Uint8Array(128).fill(0x11);
    m["fill" + n](f.subarray(5), 0x1a7); // only the low byte counts
    for (let i = 0; i < 128; i++) eq(f[i], i >= 5 && i < 5 + n ? 0xa7 : 0x11, `fill${n} byte ${i}`);
}
print("memops ok");
