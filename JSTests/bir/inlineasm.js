load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const b = n => ({ u8: n });
// Operand plain numbers are varuints; bytes are {u8}.
// const BYTE* selectAddr(U32 index, U32 lowLimit, const BYTE* candidate, const BYTE* backup)
//   asm("cmp %1, %2; cmova %3, %0" : "+r"(candidate) : "r"(index), "r"(lowLimit), "r"(backup))
//   with %0 = rax, %1 = esi... the frontend picks: candidate rax(0), index rsi(6), lowLimit rdx(2), backup rcx(1)
//   cmp %esi, %edx  = 39 f2 ;  cmova %rcx, %rax = 48 0f 47 c1
const code = [0x39, 0xf2, 0x48, 0x0f, 0x47, 0xc1];
// uint64 mulhi via "mulq": asm("mulq %3" : "=a"(lo), "=d"(hi) : "a"(x), "r"(y)) ; y in rcx: 48 f7 e1
const mul = [0x48, 0xf7, 0xe1];
const m = $vm.cModule(assemble({
    sigs: [{ ret: T.i64, params: [T.i32, T.i32, T.i64, T.i64] }, { ret: T.i64, params: [T.i64, T.i64] }],
    funcs: [
        { name: "select", sig: 0, exported: true, blocks: [[
            ["InlineAsm", b(0), code.length, ...code.map(b), 4, 2, b(0), 0, b(6), 1, b(2), 3, b(1), 1, b(T.i64), b(0), 0],
            ["Ret", 4]]] },
        { name: "mulhi", sig: 1, exported: true, blocks: [[
            ["InlineAsm", b(0), mul.length, ...mul.map(b), 2, 0, b(0), 1, b(1), 2, b(T.i64), b(0), b(T.i64), b(2), 0],
            ["Ret", 3]]] },
        { name: "mullo", sig: 1, exported: true, blocks: [[
            ["InlineAsm", b(0), mul.length, ...mul.map(b), 2, 0, b(0), 1, b(1), 2, b(T.i64), b(0), b(T.i64), b(2), 0],
            ["Ret", 2]]] },
    ],
    exports: [
        { name: "select", func: 0, ret: FFI.u64, args: [FFI.u32, FFI.u32, FFI.u64, FFI.u64] },
        { name: "mulhi", func: 1, ret: FFI.u64, args: [FFI.u64, FFI.u64] },
        { name: "mullo", func: 2, ret: FFI.u64, args: [FFI.u64, FFI.u64] },
    ],
}));
// cmp index(esi), lowLimit(edx) computes lowLimit - index; cmova (lowLimit > index unsigned) picks backup
eq(m.select(5, 3, 111n, 222n), 111n, "index >= lowLimit keeps the candidate");
eq(m.select(3, 5, 111n, 222n), 222n, "index < lowLimit takes the backup");
eq(m.select(5, 5, 111n, 222n), 111n, "equal keeps the candidate");
eq(m.mulhi(0xffffffffffffffffn, 0xffffffffffffffffn), 0xfffffffffffffffen, "mulq high half");
eq(m.mullo(0xffffffffffffffffn, 2n), 0xfffffffffffffffen, "mulq low half");
let t = 0n; for (let i = 0; i < 200000; i++) t += m.select(i & 7, 4, 1n, 2n); eq(t, 100000n * 2n + 100000n, "hot");
print("inline asm ok");
