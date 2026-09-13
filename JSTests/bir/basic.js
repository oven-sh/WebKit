load("./asm.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }

const s = n => ({ s: n });
const bytes = assemble({
    sigs: [
        { ret: T.i32, params: [T.i32, T.i32] },   // 0: int(int,int)
        { ret: T.i32, params: [T.i32] },          // 1: int(int)
        { ret: T.f64, params: [T.f64, T.i32] },   // 2: double(double,int)
    ],
    funcs: [
        // int add(int a, int b) { return a + b; }
        { name: "add", sig: 0, exported: true, blocks: [[["Add", 0, 1], ["Ret", 2]]] },
        // int fib(int n) { if (n < 2) return n; return fib(n-1) + fib(n-2); }
        { name: "fib", sig: 1, exported: true, blocks: [
            [["ConstI32", s(2)], ["Lt", 0, 1], ["Br", 2, 1, 2]],
            [["Ret", 0]],
            [["ConstI32", s(1)], ["Sub", 0, 3], ["Call", 1, 1, 4], ["ConstI32", s(2)], ["Sub", 0, 6], ["Call", 1, 1, 7], ["Add", 5, 8], ["Ret", 9]],
        ] },
        // int sum(int n) { int t = 0; for (int i = 0; i < n; i++) t += i; return t; }   locals: 0=t 1=i
        { name: "sum", sig: 1, exported: true, locals: [T.i32, T.i32], blocks: [
            [["Jump", 1]],
            [["LocalGet", 1], ["Lt", 1, 0], ["Br", 2, 2, 3]],
            [["LocalGet", 0], ["LocalGet", 1], ["Add", 3, 4], ["LocalSet", 0, 5], ["ConstI32", s(1)], ["Add", 4, 6], ["LocalSet", 1, 7], ["Jump", 1]],
            [["LocalGet", 0], ["Ret", 8]],
        ] },
        // double scale(double x, int k) { double buf[1]; buf[0] = x * k; return buf[0]; }
        { name: "scale", sig: 2, exported: true, slots: [{ size: 8, align: 8 }], blocks: [
            [["SToF", { u8: T.f64 }, 1], ["Mul", 0, 2], ["SlotAddr", 0], ["Store", { u8: MEM.f64 }, 3, 4, s(0)], ["SlotAddr", 0], ["Load", { u8: MEM.f64 }, 5, s(0)], ["Ret", 6]],
        ] },
    ],
    exports: [
        { name: "add", func: 0, ret: FFI.i32, args: [FFI.i32, FFI.i32] },
        { name: "fib", func: 1, ret: FFI.i32, args: [FFI.i32] },
        { name: "sum", func: 2, ret: FFI.i32, args: [FFI.i32] },
        { name: "scale", func: 3, ret: FFI.f64, args: [FFI.f64, FFI.i32] },
    ],
});

const m = $vm.cModule(bytes);
eq(m.add(2, 3), 5, "add");
eq(m.add(-7, 3), -4, "add neg");
eq(m.fib(20), 6765, "fib");
eq(m.sum(1000), 499500, "sum");
eq(m.scale(1.5, 4), 6, "scale");

let t = 0;
for (let i = 0; i < 1e6; i++) t = (t + m.add(i, 1)) | 0;
eq(t, (1e6 * (1e6 - 1) / 2 + 1e6) | 0, "hot loop");
print("ok", JSON.stringify($vm.ffiCompileCounts ? $vm.ffiCompileCounts() : null));
