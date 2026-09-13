load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const bytes = assemble({
    sigs: [{ ret: [T.i64, T.i64], params: [T.i64, T.i64] }, { ret: T.i64, params: [T.i64, T.i64] }],
    externs: [{ name: "ldiv", sig: 0 }],
    funcs: [
        // long f(long a, long b) { ldiv_t r = ldiv(a, b); return r.quot * 1000 + r.rem; }
        { name: "f", sig: 1, exported: true, blocks: [[["CallExtern", 0, 2, 0, 1], ["ConstI64", s(1000)], ["Mul", 2, 4], ["Add", 5, 3], ["Ret", 6]]] },
    ],
    exports: [{ name: "f", func: 0, ret: FFI.i64, args: [FFI.i64, FFI.i64] }],
});
const m = $vm.cModule(bytes);
eq(m.f(47n, 5n), 9002n, "ldiv(47, 5) = {9, 2} returned in rax:rdx by glibc");
eq(m.f(-47n, 5n), -9002n, "ldiv(-47, 5) = {-9, -2}");
print("ldiv ok");
