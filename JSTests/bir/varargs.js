load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
const consts = [];
for (let i = 1; i <= 10; i++) consts.push(["ConstI32", s(i * 3)]);
const bytes = assemble({
    sigs: [
        { ret: T.i64, params: [T.i32], variadic: true }, // 0: long sum(int n, ...)
        { ret: T.i64, params: [] },                      // 1: long(void)
        { ret: T.i32, params: [T.i32] },                 // 2: int(int)
        { ret: T.i64, params: [T.i64] },                 // 3: long(long)
    ],
    funcs: [
        { name: "sum", sig: 0, exported: false, locals: [T.i64, T.i32, T.i64], slots: [{ size: 24, align: 8 }], blocks: [
            [["SlotAddr", 0], ["VaStart", 1], ["Jump", 1]],
            [["LocalGet", 1], ["Lt", 2, 0], ["Br", 3, 2, 6]],
            [["SlotAddr", 0], ["Load", t8(MEM.i32), 4, s(0)], ["ConstI32", s(48)], ["ULt", 5, 6], ["Br", 7, 3, 4]],
            [["SlotAddr", 0], ["Load", t8(MEM.i32), 8, s(0)], ["Load", t8(MEM.i64), 8, s(16)], ["ZExt32", 9], ["Add", 10, 11], ["LocalSet", 2, 12],
             ["ConstI32", s(8)], ["Add", 9, 13], ["Store", t8(MEM.i32), 14, 8, s(0)], ["Jump", 5]],
            [["SlotAddr", 0], ["Load", t8(MEM.i64), 15, s(8)], ["LocalSet", 2, 16], ["ConstI64", s(8)], ["Add", 16, 17], ["Store", t8(MEM.i64), 18, 15, s(8)], ["Jump", 5]],
            [["LocalGet", 2], ["Load", t8(MEM.i32), 19, s(0)], ["SExt32", 20], ["LocalGet", 0], ["Add", 22, 21], ["LocalSet", 0, 23],
             ["LocalGet", 1], ["ConstI32", s(1)], ["Add", 24, 25], ["LocalSet", 1, 26], ["Jump", 1]],
            [["LocalGet", 0], ["Ret", 27]],
        ] },
        // long call10(void) { return sum(10, 3, 6, ..., 30); }
        { name: "call10", sig: 1, exported: true, blocks: [[["ConstI32", s(10)], ...consts, ["Call", 0, 11, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10], ["Ret", 11]]] },
        // long call3(void) { return sum(3, 100, -7, 5); }
        { name: "call3", sig: 1, exported: true, blocks: [[["ConstI32", s(3)], ["ConstI32", s(100)], ["ConstI32", s(-7)], ["ConstI32", s(5)], ["Call", 0, 4, 0, 1, 2, 3], ["Ret", 4]]] },
        { name: "clz", sig: 2, exported: true, blocks: [[["Clz", 0], ["Ret", 1]]] },
        { name: "ctz", sig: 2, exported: true, blocks: [[["Ctz", 0], ["Ret", 1]]] },
        { name: "popcnt", sig: 2, exported: true, blocks: [[["Popcnt", 0], ["Ret", 1]]] },
        { name: "bswap", sig: 2, exported: true, blocks: [[["Bswap", 0], ["Ret", 1]]] },
        { name: "bswap64", sig: 3, exported: true, blocks: [[["Bswap", 0], ["Ret", 1]]] },
        { name: "popcnt64", sig: 3, exported: true, blocks: [[["Popcnt", 0], ["Ret", 1]]] },
    ],
    exports: [
        { name: "call10", func: 1, ret: FFI.i64, args: [] },
        { name: "call3", func: 2, ret: FFI.i64, args: [] },
        { name: "clz", func: 3, ret: FFI.i32, args: [FFI.i32] },
        { name: "ctz", func: 4, ret: FFI.i32, args: [FFI.i32] },
        { name: "popcnt", func: 5, ret: FFI.i32, args: [FFI.i32] },
        { name: "bswap", func: 6, ret: FFI.u32, args: [FFI.u32] },
        { name: "bswap64", func: 7, ret: FFI.u64, args: [FFI.u64] },
        { name: "popcnt64", func: 8, ret: FFI.i64, args: [FFI.u64] },
    ],
});
const m = $vm.cModule(bytes);
eq(m.call10(), 165n, "sum of 10 varargs (5 in registers, 5 on the stack)");
eq(m.call3(), 98n, "sum of 3 varargs");
eq(m.clz(1), 31, "clz"); eq(m.clz(0x00f00000), 8, "clz mid");
eq(m.ctz(0x80), 7, "ctz"); eq(m.ctz(-2147483648), 31, "ctz top");
eq(m.popcnt(0xff00ff), 16, "popcnt"); eq(m.popcnt(-1), 32, "popcnt all");
eq(m.bswap(0x11223344), 0x44332211, "bswap");
eq(m.bswap64(0x1122334455667788n), 0x8877665544332211n, "bswap64");
eq(m.popcnt64(0xffffffffffffffffn), 64n, "popcnt64");
print("varargs ok");
