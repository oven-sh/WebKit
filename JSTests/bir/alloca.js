load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
const module = assemble({
    sigs: [
        { ret: T.i64, params: [T.i32] },                 // 0: long f(int n)
        { ret: T.i64, params: [T.i64, T.i32, T.i64] },   // 1: memset
        { ret: T.i64, params: [T.i32, T.i32] },          // 2: long g(int rounds, int n)
        { ret: T.i32, params: [T.i64, T.i64, T.i64], variadic: true }, // 3: snprintf
    ],
    externs: [{ name: "memset", sig: 1 }, { name: "snprintf", sig: 3 }],
    data: { size: 8, align: 1, init: [0x25, 0x64, 0x21, 0x25, 0x64, 0, 0, 0], relocs: [] },   // "%d!%d"
    funcs: [
        // long sum_squares(int n) { long v[n]; for (i<n) v[i] = i*i; long t = 0; for (i<n) t += v[i]; return t; }   locals: 0 = p (i64), 1 = i (i32), 2 = t (i64)
        { name: "sum_squares", sig: 0, exported: true, locals: [T.i64, T.i32, T.i64], blocks: [
            [["SExt32", 0], ["ConstI64", s(8)], ["Mul", 1, 2], ["StackAlloc", 3, 8], ["LocalSet", 0, 4], ["Jump", 1]],
            [["LocalGet", 1], ["Lt", 5, 0], ["Br", 6, 2, 3]],
            [["LocalGet", 1], ["SExt32", 7], ["Mul", 8, 8], ["ConstI64", s(8)], ["Mul", 8, 10], ["LocalGet", 0], ["Add", 12, 11], ["Store", t8(MEM.i64), 9, 13, s(0)],
             ["ConstI32", s(1)], ["Add", 7, 14], ["LocalSet", 1, 15], ["Jump", 1]],
            [["ConstI32", s(0)], ["LocalSet", 1, 16], ["Jump", 4]],
            [["LocalGet", 1], ["Lt", 17, 0], ["Br", 18, 5, 6]],
            [["LocalGet", 1], ["SExt32", 19], ["ConstI64", s(8)], ["Mul", 20, 21], ["LocalGet", 0], ["Add", 23, 22], ["Load", t8(MEM.i64), 24, s(0)], ["LocalGet", 2], ["Add", 26, 25], ["LocalSet", 2, 27],
             ["ConstI32", s(1)], ["Add", 19, 28], ["LocalSet", 1, 29], ["Jump", 4]],
            [["LocalGet", 2], ["Ret", 30]],
        ] },
        // long churn(int rounds, int n): each round: save sp; p = alloca(n) aligned 64; memset(p, round, n); acc += p[n-1] + (p & 63); snprintf into it (variadic call with stack args while SP is moved); restore sp.
        // returns acc * 1000000 + (sp_after - sp_before)   (must be +0: no leak across rounds)       locals: 0 = round (i32), 1 = acc (i64), 2 = saved (i64), 3 = p (i64), 4 = sp0 (i64)
        { name: "churn", sig: 2, exported: true, locals: [T.i32, T.i64, T.i64, T.i64, T.i64], blocks: [
            [["StackSave"], ["LocalSet", 4, 2], ["Jump", 1]],
            [["LocalGet", 0], ["Lt", 3, 0], ["Br", 4, 2, 3]],
            [["StackSave"], ["LocalSet", 2, 5], ["SExt32", 1], ["StackAlloc", 6, 64], ["LocalSet", 3, 7],
             ["LocalGet", 0], ["CallExtern", 0, 3, 7, 8, 6],
             ["Add", 7, 6], ["Load", t8(MEM.i8u), 10, s(-1)], ["ZExt32", 11], ["ConstI64", s(63)], ["And", 7, 13], ["Add", 12, 14], ["LocalGet", 1], ["Add", 16, 15], ["LocalSet", 1, 17],
             ["DataAddr", 0], ["ConstI32", s(7)], ["ConstI32", s(9)], ["CallExtern", 1, 5, 7, 6, 18, 19, 20],
             ["LocalGet", 2], ["StackRestore", 22],
             ["ConstI32", s(1)], ["Add", 8, 23], ["LocalSet", 0, 24], ["Jump", 1]],
            [["StackSave"], ["LocalGet", 4], ["Sub", 25, 26], ["LocalGet", 1], ["ConstI64", s(1000000)], ["Mul", 28, 29], ["Add", 30, 27], ["Ret", 31]],
        ] },
    ],
    exports: [
        { name: "sum_squares", func: 0, ret: FFI.i64, args: [FFI.i32] },
        { name: "churn", func: 1, ret: FFI.i64, args: [FFI.i32, FFI.i32] },
    ],
});
const m = $vm.cModule(module);
eq(m.sum_squares(10), 285n, "VLA of 10 longs");
eq(m.sum_squares(1000), BigInt(999 * 1000 * 1999 / 6), "VLA of 1000 longs");
eq(m.sum_squares(0), 0n, "empty VLA");
// rounds 0..9 => memset byte = round; acc = sum(round) = 45, alignment remainder must be 0, stack must be back where it started
eq(m.churn(10, 200), 45n * 1000000n, "alloca in a loop with save/restore: values right, 64-byte aligned, no stack growth");
let expected = 0; for (let r = 0; r < 100000; r++) expected += r & 255;
eq(m.churn(100000, 4096), BigInt(expected) * 1000000n, "100k rounds of 4 KiB: would overflow an 8 MiB stack 50x over without StackRestore");
print("alloca ok");
