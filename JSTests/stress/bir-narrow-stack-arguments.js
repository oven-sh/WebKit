//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// A parameter whose C type is one or two bytes wide says so in its signature (BIR.h: 6 and 7 in place of the type). Its
// value is an i32 all the same; what changes is where it goes when it goes on the stack on a target that packs stack
// arguments (Apple's AArch64): it takes its own size there. Everywhere else every argument takes 8 bytes.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });
const X86_64 = 0, ARM64 = 1, LINUX = 0, DARWIN = 1, WINDOWS = 2;
function forTarget(arch, os, module) { const bytes = assemble(module); bytes[4] = arch; bytes[5] = os; return bytes; }

// Where each argument goes, for every target, from a module that only has to decode.
{
    const longs = n => new Array(n).fill(T.i64);
    const layoutOf = (arch, os, params, anonymous = [], variadic = false) => $vm.cModuleArgumentLayout(forTarget(arch, os, {
        sigs: [{ ret: T.void, params, variadic }, { ret: T.void, params: [] }],
        funcs: [{ name: "f", sig: 1, exported: true, blocks: [[["RetVoid"]]] }],
    }), 0, anonymous).join(", ");
    const narrow = [T.i8, T.i8, T.i16, T.i32, T.i8, T.i64, T.i16, T.f32, T.i8];
    // Apple's AArch64: eight integer registers (a float goes in a register of its own kind), then the stack, packed.
    eq(layoutOf(ARM64, DARWIN, [...longs(8), ...narrow]),
        "gpr 0, gpr 1, gpr 2, gpr 3, gpr 4, gpr 5, gpr 6, gpr 7, stack 0 1, stack 1 1, stack 2 2, stack 4 4, stack 8 1, stack 16 8, stack 24 2, fpr 0, stack 26 1, total 32", "Apple arm64, narrow arguments on the stack");
    eq(layoutOf(ARM64, DARWIN, [...longs(7), T.i8, T.i8, T.i32, T.i8]), "gpr 0, gpr 1, gpr 2, gpr 3, gpr 4, gpr 5, gpr 6, gpr 7, stack 0 1, stack 4 4, stack 8 1, total 16", "Apple arm64, the last register taken by a narrow argument");
    eq(layoutOf(ARM64, DARWIN, [...longs(8), T.i16, T.i8, T.i16]), "gpr 0, gpr 1, gpr 2, gpr 3, gpr 4, gpr 5, gpr 6, gpr 7, stack 0 2, stack 2 1, stack 4 2, total 16", "Apple arm64, a short after a char");
    // What a variadic function's parameters do not name goes on the stack in 8-byte slots, after the named ones.
    eq(layoutOf(ARM64, DARWIN, [...longs(8), T.i8, T.i16], [T.i32, T.f64, T.i64], true),
        "gpr 0, gpr 1, gpr 2, gpr 3, gpr 4, gpr 5, gpr 6, gpr 7, stack 0 1, stack 2 2, stack 8 4, stack 16 8, stack 24 8, total 32", "Apple arm64, anonymous arguments");
    eq(layoutOf(ARM64, DARWIN, [T.i64], [T.i32, T.f64], true), "gpr 0, stack 0 4, stack 8 8, total 16", "Apple arm64, anonymous arguments with registers to spare");
    eq(layoutOf(ARM64, DARWIN, [{ sret: true }, ...longs(8), T.i8]), "x8, gpr 0, gpr 1, gpr 2, gpr 3, gpr 4, gpr 5, gpr 6, gpr 7, stack 0 1, total 16", "Apple arm64, an indirect result is in x8");
    // Every other target gives a stack argument 8 bytes, narrow or not.
    eq(layoutOf(ARM64, LINUX, [...longs(8), T.i8, T.i16, T.i32]), "gpr 0, gpr 1, gpr 2, gpr 3, gpr 4, gpr 5, gpr 6, gpr 7, stack 0 4, stack 8 4, stack 16 4, total 32", "Linux arm64");
    eq(layoutOf(X86_64, LINUX, [...longs(6), T.i8, T.i16, T.i32, T.i64, T.i8]), "gpr 0, gpr 1, gpr 2, gpr 3, gpr 4, gpr 5, stack 0 4, stack 8 4, stack 16 4, stack 24 8, stack 32 4, total 48", "Linux x86-64");
    eq(layoutOf(X86_64, DARWIN, [...longs(6), T.i8, T.i16]), "gpr 0, gpr 1, gpr 2, gpr 3, gpr 4, gpr 5, stack 0 4, stack 8 4, total 16", "macOS x86-64");
    eq(layoutOf(X86_64, WINDOWS, [T.i64, T.f64, T.i8, T.i16, T.i8, T.i16, T.f32]), "gpr 0, fpr 1, gpr 2, gpr 3, stack 32 4, stack 40 4, stack 48 4, total 64", "Windows x86-64");
    eq(layoutOf(X86_64, LINUX, [T.i64], [T.i32, T.f64], true), "gpr 0, gpr 1, fpr 0, total 0", "Linux x86-64, anonymous arguments");
}

// long callee(long a0, ..., long a7, char c0, char c1, short s0, int i0, char c2, long l0, short s1)
// { return a7 + c0 + c1 * 3 + s0 * 5 + i0 * 7 + c2 * 11 + l0 * 13 + s1 * 17; }     the callee extends what it is given
// long caller(long x) { return callee(1, ..., 8, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6); }     with more in the upper bits than belongs there
const FIRST = 8;
const kinds = [["i8", 8], ["i8", 8], ["i16", 16], ["i32", 32], ["i8", 8], ["i64", 64], ["i16", 16]];
const weights = [1, 3, 5, 7, 11, 13, 17];
const callee = new Block(FIRST + kinds.length);
let total = FIRST - 1;
kinds.forEach(([, bits], i) => {
    let value = FIRST + i;
    if (bits === 8)
        value = callee.def("SExt8", value);
    else if (bits === 16)
        value = callee.def("SExt16", value);
    if (bits !== 64)
        value = callee.def("SExt32", value);
    total = callee.def("Add", total, callee.def("Mul", value, callee.def("ConstI64", s(weights[i]))));
});
callee.run("Ret", total);
const caller = new Block(1);
const operands = [];
for (let i = 1; i <= FIRST; i++)
    operands.push(caller.def("ConstI64", s(i)));
kinds.forEach(([, bits], i) => {
    const wide = caller.def("Add", 0, caller.def("ConstI64", s(i)));
    // The bits above the argument's own are not the callee's to look at: they are given something that is not the sign.
    operands.push(bits === 64 ? wide : caller.def("Xor", caller.def("Trunc", wide), caller.def("ConstI32", s(bits === 32 ? 0 : 0x5a5a0000 | (bits === 8 ? 0xa500 : 0)))));
});
caller.run("Ret", caller.def("Call", 0, operands.length, ...operands));
const c = $vm.cModule(assemble({
    sigs: [{ ret: T.i64, params: [...new Array(FIRST).fill(T.i64), ...kinds.map(([name]) => T[name])] }, { ret: T.i64, params: [T.i64] }],
    funcs: [{ name: "callee", sig: 0, exported: true, noinline: true, blocks: [callee.insts] }, { name: "caller", sig: 1, exported: true, blocks: [caller.insts] }],
    exports: [
        { name: "callee", func: 0, ret: FFI.i64, args: [...new Array(FIRST).fill(FFI.i64), ...kinds.map(([name]) => FFI[name])] },
        { name: "caller", func: 1, ret: FFI.i64, args: [FFI.i64] },
    ],
}));
function reference(x) {
    let sum = BigInt(FIRST);
    kinds.forEach(([, bits], i) => { sum += BigInt.asIntN(bits, BigInt(x + i)) * BigInt(weights[i]); });
    return BigInt.asIntN(64, sum);
}
for (const x of [0, 1, -1, 100, 126, 127, 128, 255, 256, -128, -129, 32767, 32768, 65535, 65536, 2147483647, -2147483648]) {
    eq(c.caller(x), reference(x), `compiled C calling compiled C, x = ${x}`);
    // From JavaScript, which passes each argument as its declared type.
    const given = kinds.map(([, bits], i) => bits === 64 ? BigInt(x + i) : Number(BigInt.asIntN(bits, BigInt(x + i))));
    eq(c.callee(1n, 2n, 3n, 4n, 5n, 6n, 7n, 8n, ...given), reference(x), `JavaScript calling compiled C, x = ${x}`);
}
function hot(n) { let sum = 0n; for (let i = 0; i < n; i++) sum += c.caller(i & 1023); return sum; }
noInline(hot);
let expected = 0n;
for (let i = 0; i < 100000; i++)
    expected += reference(i & 1023);
eq(hot(100000), expected, "hot");
print("narrow stack arguments ok");
