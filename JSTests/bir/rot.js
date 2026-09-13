load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const m = $vm.cModule(assemble({
    sigs: [{ ret: T.i64, params: [T.i64, T.i32] }, { ret: T.i32, params: [T.i32, T.i32] }],
    funcs: [
        { name: "rotl64", sig: 0, exported: true, blocks: [[["RotL", 0, 1], ["Ret", 2]]] },
        { name: "rotr64", sig: 0, exported: true, blocks: [[["RotR", 0, 1], ["Ret", 2]]] },
        { name: "rotl32", sig: 1, exported: true, blocks: [[["RotL", 0, 1], ["Ret", 2]]] },
        { name: "rotr32", sig: 1, exported: true, blocks: [[["RotR", 0, 1], ["Ret", 2]]] },
    ],
    exports: [
        { name: "rotl64", func: 0, ret: FFI.u64, args: [FFI.u64, FFI.i32] }, { name: "rotr64", func: 1, ret: FFI.u64, args: [FFI.u64, FFI.i32] },
        { name: "rotl32", func: 2, ret: FFI.u32, args: [FFI.u32, FFI.i32] }, { name: "rotr32", func: 3, ret: FFI.u32, args: [FFI.u32, FFI.i32] },
    ],
}));
const M = (1n << 64n) - 1n;
for (const n of [0, 1, 13, 63, 64, 65, -1]) {
    const k = BigInt(((n % 64) + 64) % 64), x = 0x0123456789abcdefn;
    eq(m.rotl64(x, n), ((x << k) | (x >> (64n - k))) & M, "rotl64 " + n);
    eq(m.rotr64(x, n), ((x >> k) | (x << (64n - k))) & M, "rotr64 " + n);
    const j = ((n % 32) + 32) % 32, y = 0x89abcdef;
    eq(m.rotl32(y, n), ((y << j) | (y >>> ((32 - j) & 31))) >>> 0, "rotl32 " + n);
    eq(m.rotr32(y, n), ((y >>> j) | (y << ((32 - j) & 31))) >>> 0, "rotr32 " + n);
}
print("rotate ok");
