//@ skip if !$isFTLPlatform
//@ skip if $architecture != "x86_64"
//@ requireOptions("--useDollarVM=1")

load("./resources/bir-assembler.js", "caller relative");
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

// What a statement says about memory (BIR.h, InlineAsm's flags) is what keeps it where it is written.
{
    const s = n => ({ s: n });
    const HAS_EFFECTS = 1, READS = 2, WRITES = 4;
    const RAX = 0, RSI = 6, RDI = 7;
    const load = [0x8b, 0x07]; // movl (%rdi), %eax       asm("movl %1, %0" : "=r"(result) : "m"(*p))
    const store = [0x89, 0x37]; // movl %esi, (%rdi)      asm("movl %1, %0" : "=m"(*p) : "r"(value))
    const asmLoad = (flags, pointer) => ["InlineAsm", b(flags), load.length, ...load.map(b), 1, pointer, b(RDI), 1, b(T.i32), b(RAX), 0];
    const asmStore = (flags, pointer, value) => ["InlineAsm", b(flags), store.length, ...store.map(b), 2, pointer, b(RDI), value, b(RSI), 0, 0];
    // int sumOfLoads(int* p, int n) { int i = 0, sum = 0; do { *p = i; sum += <the load>(p); i++; } while (i < n); return sum; }
    // Locals: 0 i, 1 sum. The store is B3's own; the load is the statement.
    const sumOfLoads = flags => [
        [["Jump", 1]],
        [["LocalGet", 0], ["Store", b(MEM.i32), 2, 0, s(0)], asmLoad(flags, 0), ["LocalGet", 1], ["Add", 4, 3], ["LocalSet", 1, 5],
            ["ConstI32", s(1)], ["Add", 2, 6], ["LocalSet", 0, 7], ["Lt", 7, 1], ["Br", 8, 1, 2]],
        [["LocalGet", 1], ["Ret", 9]],
    ];
    // int sumOfStores(int* p, int n) { int i = 0, sum = 0; do { <the store>(p, i); sum += *p; i++; } while (i < n); return sum; }
    const sumOfStores = flags => [
        [["Jump", 1]],
        [["LocalGet", 0], asmStore(flags, 0, 2), ["Load", b(MEM.i32), 0, s(0)], ["LocalGet", 1], ["Add", 4, 3], ["LocalSet", 1, 5],
            ["ConstI32", s(1)], ["Add", 2, 6], ["LocalSet", 0, 7], ["Lt", 7, 1], ["Br", 8, 1, 2]],
        [["LocalGet", 1], ["Ret", 9]],
    ];
    // int twice(int* p) { int a = <the load>(p); *p = a + 1; int b = <the same load>(p); return a * 100 + b; }
    const twice = flags => [[asmLoad(flags, 0), ["ConstI32", s(1)], ["Add", 1, 2], ["Store", b(MEM.i32), 3, 0, s(0)], asmLoad(flags, 0),
        ["ConstI32", s(100)], ["Mul", 1, 5], ["Add", 6, 4], ["Ret", 7]]];
    const funcs = [], exports = [];
    const add = (name, sig, blocks, args) => {
        funcs.push({ name, sig, exported: true, locals: [T.i32, T.i32], blocks });
        exports.push({ name, func: funcs.length - 1, ret: FFI.i32, args });
    };
    for (const flags of [READS, READS | WRITES, HAS_EFFECTS, HAS_EFFECTS | READS | WRITES]) {
        add("sumOfLoads" + flags, 0, sumOfLoads(flags), [FFI.ptr, FFI.i32]);
        add("twice" + flags, 1, twice(flags), [FFI.ptr]);
    }
    for (const flags of [WRITES, READS | WRITES, HAS_EFFECTS, HAS_EFFECTS | WRITES])
        add("sumOfStores" + flags, 0, sumOfStores(flags), [FFI.ptr, FFI.i32]);
    // With no flag the statement is a function of its operands, the pointer: it may run once for the whole loop, or not at all.
    add("sumOfLoads0", 0, sumOfLoads(0), [FFI.ptr, FFI.i32]);
    add("sumOfStores0", 0, sumOfStores(0), [FFI.ptr, FFI.i32]);
    const c = $vm.cModule(assemble({ sigs: [{ ret: T.i32, params: [T.i64, T.i32] }, { ret: T.i32, params: [T.i64] }], funcs, exports }));
    const cell = new Int32Array(new ArrayBuffer(16));
    for (const flags of [READS, READS | WRITES, HAS_EFFECTS, HAS_EFFECTS | READS | WRITES]) {
        cell[0] = 7210;
        eq(c["sumOfLoads" + flags](cell, 10), 45, `a load that says ${flags} in a loop that stores`);
        cell[0] = 41;
        eq(c["twice" + flags](cell), 4142, `two loads that say ${flags} around a store`);
    }
    for (const flags of [WRITES, READS | WRITES, HAS_EFFECTS, HAS_EFFECTS | WRITES]) {
        cell[0] = 7210;
        eq(c["sumOfStores" + flags](cell, 10), 45, `a store that says ${flags} in a loop that loads`);
        eq(cell[0], 9, `a store that says ${flags}: what it left`);
    }
    cell[0] = 3;
    c.sumOfLoads0(cell, 10);
    c.sumOfStores0(cell, 10);
}
print("inline asm ok");
