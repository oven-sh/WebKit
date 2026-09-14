//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// A call made after the stack pointer has moved (C's alloca, a VLA) passes its stack arguments above the stack
// pointer as it is then, not where the frame's outgoing-argument area was when the function was entered.
load("./resources/bir-assembler.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n }), t8 = n => ({ u8: n });
const COUNT = 12; // more than either ABI passes in registers

// long weighted(long a0, ..., long a11) { return a0*1 + a1*2 + ... + a11*12; }
const weighted = [];
let next = COUNT, sum = null;
for (let i = 0; i < COUNT; i++) {
    weighted.push(["ConstI64", s(i + 1)]); const weight = next++;
    weighted.push(["Mul", i, weight]); const term = next++;
    if (sum === null) sum = term; else { weighted.push(["Add", sum, term]); sum = next++; }
}
weighted.push(["Ret", sum]);

// long caller(long n) { char *p = alloca(n); p[0] = 7; return weighted(10, 20, ..., 120) + p[0]; }
const caller = [["StackAlloc", 0, 16], ["ConstI32", s(7)], ["Store", t8(MEM.i8u), 2, 1, s(0)]];
let id = 3; const args = [];
for (let i = 0; i < COUNT; i++) { caller.push(["ConstI64", s((i + 1) * 10)]); args.push(id++); }
caller.push(["Call", 0, COUNT, ...args]); const result = id++;
caller.push(["Load", t8(MEM.i8u), 1, s(0)]); const byte = id++;
caller.push(["ZExt32", byte]); const wide = id++;
caller.push(["Add", result, wide]); const total = id++;
caller.push(["Ret", total]);

const m = $vm.cModule(assemble({
    sigs: [{ ret: T.i64, params: Array(COUNT).fill(T.i64) }, { ret: T.i64, params: [T.i64] }],
    funcs: [
        { name: "weighted", sig: 0, exported: false, noinline: true, blocks: [weighted] },
        { name: "caller", sig: 1, exported: true, blocks: [caller] },
    ],
    exports: [{ name: "caller", func: 1, ret: FFI.i64, args: [FFI.i64] }],
}));
let expected = 7n;
for (let i = 0; i < COUNT; i++) expected += BigInt((i + 1) * 10 * (i + 1));
for (const n of [1, 16, 17, 100, 4096]) eq(m.caller(n), expected, `stack arguments after alloca(${n})`);

// The same with an aggregate passed by value first, large enough that what follows it in the outgoing arguments is
// further from the stack pointer than an ARM64 load or store can say in its offset (32760), or an add in its immediate.
// long last(struct { char bytes[SIZE]; } big, long a0, ..., long a11) { return big.bytes[SIZE - 1] + a11; }
// long caller(long n) { struct big local; char* p = alloca(n); p[0] = 7; local.bytes[SIZE - 1] = 9; return last(local, 10, ..., 120) + p[0]; }
// (Win64 passes every aggregate by reference.)
for (const SIZE of $vm.cModuleHost()[1] === 2 ? [] : [4000, 40000, 1 << 20]) {
    const last = [["Load", t8(MEM.i8u), 0, s(SIZE - 1)], ["ZExt32", COUNT + 1], ["Add", COUNT + 2, COUNT], ["Ret", COUNT + 3]];
    const callerOfLast = [["StackAlloc", 0, 16], ["ConstI32", s(7)], ["Store", t8(MEM.i8u), 2, 1, s(0)],
        ["SlotAddr", 0], ["ConstI32", s(9)], ["Store", t8(MEM.i8u), 4, 3, s(SIZE - 1)]];
    let id = 5;
    const longs = [];
    for (let i = 0; i < COUNT; i++) { callerOfLast.push(["ConstI64", s((i + 1) * 10)]); longs.push(id++); }
    callerOfLast.push(["Call", 0, COUNT + 1, 3, ...longs]); const result = id++;
    callerOfLast.push(["Load", t8(MEM.i8u), 1, s(0)]); const byte = id++;
    callerOfLast.push(["ZExt32", byte]); const wide = id++;
    callerOfLast.push(["Add", result, wide]); const total = id++;
    callerOfLast.push(["Ret", total]);
    const large = $vm.cModule(assemble({
        sigs: [{ ret: T.i64, params: [{ byval: SIZE }, ...Array(COUNT).fill(T.i64)] }, { ret: T.i64, params: [T.i64] }],
        funcs: [
            { name: "last", sig: 0, noinline: true, blocks: [last] },
            { name: "caller", sig: 1, exported: true, slots: [{ size: SIZE, align: 8 }], blocks: [callerOfLast] },
        ],
        exports: [{ name: "caller", func: 1, ret: FFI.i64, args: [FFI.i64] }],
    }));
    for (const n of [1, 100, 4096, 70000])
        eq(large.caller(n), BigInt(9 + COUNT * 10 + 7), `an aggregate of ${SIZE} bytes and stack arguments after alloca(${n})`);
}
print("alloca then call ok");
