//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// Calls of the C library's fabs, floor, ceil, trunc and sqrt (and their float forms) become the instruction.
// What comes back is what the library gives, bit for bit, for every kind of argument, and sqrt of a negative
// number still sets errno.
load("./resources/bir-assembler.js", "caller relative");

function same(a, b, what) { if (!Object.is(a, b)) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });

const names = ["fabs", "floor", "ceil", "trunc", "sqrt"];
const reference = { fabs: Math.abs, floor: Math.floor, ceil: Math.ceil, trunc: Math.trunc, sqrt: Math.sqrt };
const externs = [], funcs = [], exports = [];
for (const [suffix, type, ffi, sig] of [["", T.f64, FFI.f64, 0], ["f", T.f32, FFI.f32, 1]]) {
    for (const name of names) {
        externs.push({ name: name + suffix, sig });
        exports.push({ name: name + suffix, func: funcs.length, ret: ffi, args: [ffi] });
        funcs.push({ name: name + suffix, sig, exported: true, blocks: [[["CallExtern", externs.length - 1, 1, 0], ["Ret", 1]]] });
    }
}
// double hypotenuse(double a, double b) { return sqrt(a * a + b * b); }   with the call in the middle of a block
exports.push({ name: "hypotenuse", func: funcs.length, ret: FFI.f64, args: [FFI.f64, FFI.f64] });
funcs.push({ name: "hypotenuse", sig: 2, exported: true, blocks: [[["Mul", 0, 0], ["Mul", 1, 1], ["Add", 2, 3], ["CallExtern", 4, 1, 4], ["Add", 5, 2], ["Sub", 6, 2], ["Ret", 7]]] });
// int lastError(void) { return errno; }     void clearError(void) { errno = 0; }
const errnoLocation = $vm.cModuleHost()[1] === 1 ? "__error" : "__errno_location";
externs.push({ name: errnoLocation, sig: 3 });
exports.push({ name: "lastError", func: funcs.length, ret: FFI.i32, args: [] });
funcs.push({ name: "lastError", sig: 4, exported: true, blocks: [[["CallExtern", externs.length - 1, 0], ["Load", { u8: MEM.i32 }, 0, s(0)], ["Ret", 1]]] });
exports.push({ name: "clearError", func: funcs.length, ret: FFI.void, args: [] });
funcs.push({ name: "clearError", sig: 5, exported: true, blocks: [[["CallExtern", externs.length - 1, 0], ["ConstI32", s(0)], ["Store", { u8: MEM.i32 }, 1, 0, s(0)], ["RetVoid"]]] });
// A function of the program's own called sqrt, with another type, is just a function.
externs.push({ name: "labs", sig: 6 });

const m = $vm.cModule(assemble({
    sigs: [{ ret: T.f64, params: [T.f64] }, { ret: T.f32, params: [T.f32] }, { ret: T.f64, params: [T.f64, T.f64] }, { ret: T.i64, params: [] }, { ret: T.i32, params: [] }, { ret: T.void, params: [] }, { ret: T.i64, params: [T.i64] }],
    externs, funcs, exports,
}));

const values = [0, -0, 1, -1, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 0.49999999999999994, 4, 2, 1e-310, -1e-310, 5e-324, 4503599627370495.5, 4503599627370496, -4503599627370496.5, 9007199254740993, 1e300, -1e300, Infinity, -Infinity, NaN, 123456.789, -123456.789];
for (const name of names) {
    for (const value of values) {
        same(m[name](value), reference[name](value), `${name}(${value})`);
        const single = Math.fround(value);
        same(m[name + "f"](single), Math.fround(reference[name](single)), `${name}f(${single})`);
    }
}
same(m.hypotenuse(3, 4), 5, "sqrt in the middle of a block");
same(m.hypotenuse(NaN, 4), NaN, "sqrt of NaN in the middle of a block");

const EDOM = 33;
m.clearError();
same(m.sqrt(4), 2, "sqrt(4)");
same(m.lastError(), 0, "sqrt(4) leaves errno alone");
same(m.sqrt(-1), NaN, "sqrt(-1)");
same(m.lastError(), EDOM, "sqrt(-1) sets errno");
m.clearError();
same(m.sqrtf(-4), NaN, "sqrtf(-4)");
same(m.lastError(), EDOM, "sqrtf(-4) sets errno");
m.clearError();
same(m.sqrt(NaN), NaN, "sqrt(NaN)");
same(m.lastError(), 0, "sqrt(NaN) leaves errno alone");

let total = 0;
for (let i = 0; i < 300000; i++) total += m.sqrt(i) + m.floor(i / 7) + m.fabs(-i) + m.ceilf(i / 3) + m.trunc(-i / 5);
let expected = 0;
for (let i = 0; i < 300000; i++) expected += Math.sqrt(i) + Math.floor(i / 7) + Math.abs(-i) + Math.fround(Math.ceil(Math.fround(i / 3))) + Math.trunc(-i / 5);
same(total, expected, "in a hot loop");
print("math functions ok");
