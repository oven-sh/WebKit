load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
// Machine-level view of:
//   struct P { long a, b; };            // 2 x INTEGER -> rax:rdx
//   struct V { double x, y; };          // 2 x SSE     -> xmm0:xmm1
//   struct M { long a; double b; };     // INTEGER+SSE -> rax, xmm0
//   struct Big { long v[4]; };          // MEMORY      -> copied onto the stack
const bytes = assemble({
    sigs: [
        { ret: [T.i64, T.i64], params: [T.i64, T.i64] },            // 0: P swap(P)
        { ret: [T.f64, T.f64], params: [T.f64, T.f64, T.f64] },     // 1: V scale(V, double)
        { ret: [T.i64, T.f64], params: [T.i64, T.f64] },            // 2: M bump(M)
        { ret: T.i64, params: [T.i64, T.i64] },                     // 3: long(long,long)
        { ret: T.f64, params: [T.f64, T.f64, T.f64] },              // 4: double(double,double,double)
        { ret: T.i64, params: [T.i32, { byval: 32 }, T.i32] },      // 5: long sumBig(int k, Big b, int m)
        { ret: T.i64, params: [T.i64] },                            // 6: long(long)
    ],
    funcs: [
        { name: "swap", sig: 0, blocks: [[["Ret", 1, 0]]] },
        { name: "scale", sig: 1, blocks: [[["Mul", 0, 2], ["Mul", 1, 2], ["Ret", 3, 4]]] },
        { name: "bump", sig: 2, blocks: [[["ConstI64", s(1)], ["Add", 0, 2], ["ConstF64", { f64: 0.5 }], ["Add", 1, 4], ["Ret", 3, 5]]] },
        // long use_swap(long a, long b) { P r = swap((P){a,b}); return r.a * 1000 + r.b; }
        { name: "use_swap", sig: 3, exported: true, blocks: [[["Call", 0, 2, 0, 1], ["ConstI64", s(1000)], ["Mul", 2, 4], ["Add", 5, 3], ["Ret", 6]]] },
        // double use_scale(double x, double y, double k) { V r = scale((V){x,y}, k); return r.x * 100 + r.y; }
        { name: "use_scale", sig: 4, exported: true, blocks: [[["Call", 1, 3, 0, 1, 2], ["ConstF64", { f64: 100 }], ["Mul", 3, 5], ["Add", 6, 4], ["Ret", 7]]] },
        // long use_bump(long a, long bbits) -> (a+1) * 10 + (long)(b + 0.5)   with b = (double)bbits
        { name: "use_bump", sig: 3, exported: true, blocks: [[["SToF", t8(T.f64), 1], ["Call", 2, 2, 0, 2], ["ConstI64", s(10)], ["Mul", 3, 5], ["FToS", t8(T.i64), 4], ["Add", 6, 7], ["Ret", 8]]] },
        // long sumBig(int k, Big b, int m) { b.v[0] += 1000; return k + b.v[0] + b.v[1] + b.v[2] + b.v[3] + m; }
        { name: "sumBig", sig: 5, blocks: [[
            ["Load", t8(MEM.i64), 1, s(0)], ["ConstI64", s(1000)], ["Add", 3, 4], ["Store", t8(MEM.i64), 5, 1, s(0)],
            ["Load", t8(MEM.i64), 1, s(8)], ["Load", t8(MEM.i64), 1, s(16)], ["Load", t8(MEM.i64), 1, s(24)],
            ["SExt32", 0], ["SExt32", 2], ["Add", 5, 6], ["Add", 11, 7], ["Add", 12, 8], ["Add", 13, 9], ["Add", 14, 10], ["Ret", 15],
        ]] },
        // long use_big(long base): Big b = {base, 2, 3, 4}; long r = sumBig(7, b, 9); return r * 10 + (b.v[0] == base);   // callee's +1000 must not be visible
        { name: "use_big", sig: 6, exported: true, slots: [{ size: 32, align: 8 }], blocks: [[
            ["SlotAddr", 0], ["Store", t8(MEM.i64), 0, 1, s(0)],
            ["ConstI64", s(2)], ["Store", t8(MEM.i64), 2, 1, s(8)], ["ConstI64", s(3)], ["Store", t8(MEM.i64), 3, 1, s(16)], ["ConstI64", s(4)], ["Store", t8(MEM.i64), 4, 1, s(24)],
            ["ConstI32", s(7)], ["ConstI32", s(9)], ["Call", 6, 3, 5, 1, 6],
            ["ConstI64", s(10)], ["Mul", 7, 8], ["Load", t8(MEM.i64), 1, s(0)], ["Eq", 10, 0], ["ZExt32", 11], ["Add", 9, 12], ["Ret", 13],
        ]] },
    ],
    exports: [
        { name: "use_swap", func: 3, ret: FFI.i64, args: [FFI.i64, FFI.i64] },
        { name: "use_scale", func: 4, ret: FFI.f64, args: [FFI.f64, FFI.f64, FFI.f64] },
        { name: "use_bump", func: 5, ret: FFI.i64, args: [FFI.i64, FFI.i64] },
        { name: "use_big", func: 7, ret: FFI.i64, args: [FFI.i64] },
    ],
});
for (const inline of [1000, 0]) {
    // with and without C->C inlining: both the patchpoint call/epilogue path and the inlined path
    const m = $vm.cModule(bytes);
    eq(m.use_swap(3n, 4n), 4003n, "two integer registers");
    eq(m.use_scale(1.5, 2.5, 2), 305, "two vector registers");
    eq(m.use_bump(41n, 7n), 427n, "integer + vector register");
    eq(m.use_big(5n), BigInt((7 + 1005 + 2 + 3 + 4 + 9) * 10 + 1), "by-value struct on the stack");
}
print("aggregates ok");
