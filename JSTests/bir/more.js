load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
const str = x => [...x].map(c => c.charCodeAt(0)).concat([0]);

// data layout: [0..6) "hello\0", pad to 8, [8..16) fmt "%d-%s-%.1f\0" (11 bytes) -> put at 8..19, pad to 24,
// [24..32) pointer to "hello" (reloc data), [32..40) pointer to func `twice` (reloc func), [40..104) char buf[64] (bss)
const init = new Array(40).fill(0);
str("hello").forEach((b, i) => init[i] = b);
str("%d-%s-%.1f").forEach((b, i) => init[8 + i] = b);

const bytes = assemble({
    sigs: [
        { ret: T.i64, params: [T.i64] },                          // 0: size_t strlen(const char*)
        { ret: T.i32, params: [T.i64, T.i64, T.i64], variadic: true }, // 1: int snprintf(char*, size_t, const char*, ...)
        { ret: T.i32, params: [] },                               // 2: int(void)
        { ret: T.i32, params: [T.i32] },                          // 3: int(int)
        { ret: T.i64, params: [] },                               // 4: ptr(void)
        { ret: T.i32, params: [T.i32, T.i32, T.i32, T.i32, T.i32, T.i32, T.i32, T.i32, T.i32, T.i32] }, // 5: 10 ints
        { ret: T.f64, params: [T.f64, T.f64, T.f64, T.f64, T.f64, T.f64, T.f64, T.f64, T.f64, T.f64] }, // 6: 10 doubles
        { ret: T.f64, params: [T.f32, T.i32] },                   // 7: double(float, unsigned)
        { ret: T.i32, params: [T.f64] },                          // 8: int(double)
    ],
    externs: [{ name: "strlen", sig: 0 }, { name: "snprintf", sig: 1 }],
    data: { size: 104, align: 8, init, relocs: [{ offset: 24, kind: 0, index: 0 }, { offset: 32, kind: 1, index: 1 }] },
    funcs: [
        // 0: int hello_len(void) { return (int)strlen(*(char**)&data[24]); }
        { name: "hello_len", sig: 2, exported: true, blocks: [[["DataAddr", 24], ["Load", t8(MEM.i64), 0, s(0)], ["CallExtern", 0, 1, 1], ["Trunc", 2], ["Ret", 3]]] },
        // 1: int twice(int x) { return x * 2; }
        { name: "twice", sig: 3, exported: true, blocks: [[["ConstI32", s(2)], ["Mul", 0, 1], ["Ret", 2]]] },
        // 2: int via_ptr(int x) { int (*fp)(int) = *(void**)&data[32]; return fp(x) + 1; }
        { name: "via_ptr", sig: 3, exported: true, blocks: [[["DataAddr", 32], ["Load", t8(MEM.i64), 1, s(0)], ["CallIndirect", 3, 2, 1, 0], ["ConstI32", s(1)], ["Add", 3, 4], ["Ret", 5]]] },
        // 3: char* fmt(void) { snprintf(buf, 64, "%d-%s-%.1f", 42, "hello", 2.5); return buf; }
        { name: "fmt", sig: 4, exported: true, blocks: [[
            ["DataAddr", 40], ["ConstI64", s(64)], ["DataAddr", 8], ["ConstI32", s(42)], ["DataAddr", 0], ["ConstF64", { f64: 2.5 }],
            ["CallExtern", 1, 6, 0, 1, 2, 3, 4, 5], ["DataAddr", 40], ["Ret", 7],
        ]] },
        // 4: int sw(int x) { switch (x) { case 1: return 10; case 5: return 50; case -3: return 30; default: return -1; } }
        { name: "sw", sig: 3, exported: true, blocks: [
            [["Switch", 0, 4, 3, s(1), 1, s(5), 2, s(-3), 3]],
            [["ConstI32", s(10)], ["Ret", 1]],
            [["ConstI32", s(50)], ["Ret", 2]],
            [["ConstI32", s(30)], ["Ret", 3]],
            [["ConstI32", s(-1)], ["Ret", 4]],
        ] },
        // 5: int ten(a..j) { return a + 2*b ... weighted so order matters: a - b + c - d + e - f + g - h + i*100 + j*1000 }
        { name: "ten", sig: 5, exported: true, blocks: [[
            ["Sub", 0, 1], ["Add", 10, 2], ["Sub", 11, 3], ["Add", 12, 4], ["Sub", 13, 5], ["Add", 14, 6], ["Sub", 15, 7],
            ["ConstI32", s(100)], ["Mul", 8, 17], ["Add", 16, 18], ["ConstI32", s(1000)], ["Mul", 9, 20], ["Add", 19, 21], ["Ret", 22],
        ]] },
        // 6: double tend(a..j) { return a - b + c - d + e - f + g - h + i*100 + j*1000; }
        { name: "tend", sig: 6, exported: true, blocks: [[
            ["Sub", 0, 1], ["Add", 10, 2], ["Sub", 11, 3], ["Add", 12, 4], ["Sub", 13, 5], ["Add", 14, 6], ["Sub", 15, 7],
            ["ConstF64", { f64: 100 }], ["Mul", 8, 17], ["Add", 16, 18], ["ConstF64", { f64: 1000 }], ["Mul", 9, 20], ["Add", 19, 21], ["Ret", 22],
        ]] },
        // 7: double conv(float f, unsigned u) { return (double)f + (double)u; }
        { name: "conv", sig: 7, exported: true, blocks: [[["FPromote", 0], ["UToF", t8(T.f64), 1], ["Add", 2, 3], ["Ret", 4]]] },
        // 8: int trunc(double d) { return (int)d; }
        { name: "trunc", sig: 8, exported: true, blocks: [[["FToS", t8(T.i32), 0], ["Ret", 1]]] },
        // 9: caller of ten through Call (outgoing stack args): int call_ten(int x) { return ten(x,1,2,3,4,5,6,7,8,9); }
        { name: "call_ten", sig: 3, exported: true, blocks: [[
            ["ConstI32", s(1)], ["ConstI32", s(2)], ["ConstI32", s(3)], ["ConstI32", s(4)], ["ConstI32", s(5)], ["ConstI32", s(6)], ["ConstI32", s(7)], ["ConstI32", s(8)], ["ConstI32", s(9)],
            ["Call", 5, 10, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9], ["Ret", 10],
        ]] },
    ],
    exports: [
        { name: "hello_len", func: 0, ret: FFI.i32, args: [] },
        { name: "twice", func: 1, ret: FFI.i32, args: [FFI.i32] },
        { name: "via_ptr", func: 2, ret: FFI.i32, args: [FFI.i32] },
        { name: "fmt", func: 3, ret: FFI.ptr, args: [] },
        { name: "sw", func: 4, ret: FFI.i32, args: [FFI.i32] },
        { name: "ten", func: 5, ret: FFI.i32, args: new Array(10).fill(FFI.i32) },
        { name: "tend", func: 6, ret: FFI.f64, args: new Array(10).fill(FFI.f64) },
        { name: "conv", func: 7, ret: FFI.f64, args: [FFI.f32, FFI.u32] },
        { name: "trunc", func: 8, ret: FFI.i32, args: [FFI.f64] },
        { name: "call_ten", func: 9, ret: FFI.i32, args: [FFI.i32] },
    ],
});
const m = $vm.cModule(bytes);
eq(m.hello_len(), 5, "strlen via reloc");
eq(m.via_ptr(20), 41, "call through func reloc");
eq($vm.ffiCString(m.fmt()), "42-hello-2.5", "variadic snprintf");
eq(m.sw(1), 10, "sw 1"); eq(m.sw(5), 50, "sw 5"); eq(m.sw(-3), 30, "sw -3"); eq(m.sw(7), -1, "sw default");
const tenJS = (a, b, c, d, e, f, g, h, i, j) => a - b + c - d + e - f + g - h + i * 100 + j * 1000;
eq(m.ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10), tenJS(1, 2, 3, 4, 5, 6, 7, 8, 9, 10), "ten ints (stack args in)");
eq(m.tend(1.5, 2, 3, 4, 5, 6, 7, 8, 9, 10), tenJS(1.5, 2, 3, 4, 5, 6, 7, 8, 9, 10), "ten doubles");
eq(m.call_ten(100), tenJS(100, 1, 2, 3, 4, 5, 6, 7, 8, 9), "ten ints (stack args out)");
eq(m.conv(1.5, 4000000000), 4000000001.5, "float + unsigned");
eq(m.trunc(-3.99), -3, "trunc");
// hot loops so every path also runs inlined / through FTL
for (let i = 0; i < 2e5; i++) {
    eq(m.sw(i % 7), [-1, 10, -1, -1, -1, 50, -1][i % 7], "sw hot");
    eq(m.ten(i, 2, 3, 4, 5, 6, 7, 8, 9, 10), tenJS(i, 2, 3, 4, 5, 6, 7, 8, 9, 10) | 0, "ten hot");
    eq(m.via_ptr(i), i * 2 + 1, "via_ptr hot");
}
eq(m.hello_len(), 5, "strlen after");
print("ok", JSON.stringify($vm.ffiCompileCounts()));
function hotSw(n) { let t = 0; for (let i = 0; i < n; i++) t += m.sw(i & 7); return t; }
function hotPtr(n) { let t = 0; for (let i = 0; i < n; i++) t = (t + m.via_ptr(i)) | 0; return t; }
function hotTen(n) { let t = 0; for (let i = 0; i < n; i++) t = (t + m.ten(i, 2, 3, 4, 5, 6, 7, 8, 9, 10)) | 0; return t; }
function hotLen(n) { let t = 0; for (let i = 0; i < n; i++) t += m.hello_len(); return t; }
for (let k = 0; k < 3; k++) {
    eq(hotSw(1e6), 125000 * (-1 + 10 - 1 - 1 - 1 + 50 - 1 - 1), "hotSw");
    let e = 0; for (let i = 0; i < 1e6; i++) e = (e + i * 2 + 1) | 0;
    eq(hotPtr(1e6), e, "hotPtr");
    e = 0; for (let i = 0; i < 1e6; i++) e = (e + tenJS(i, 2, 3, 4, 5, 6, 7, 8, 9, 10)) | 0;
    eq(hotTen(1e6), e, "hotTen");
    eq(hotLen(1e6), 5e6, "hotLen");
}
print("hot ok", JSON.stringify($vm.ffiCompileCounts()));
