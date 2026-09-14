//@ skip if !$isFTLPlatform
//@ skip if $hostOS == "windows"
//@ requireOptions("--useDollarVM=1")

// A function that calls setjmp is entered a second time at that call, by longjmp, after code that the control flow
// graph says never leads there. What the function computed before the call and still needs after the second return
// is there: in the registers longjmp restores, or in stack slots that nothing computed since was given.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });

// static jmp_buf buffer;
// noinline long opaque(long x) { return x * 7 + 1; }
// noinline void thrower0(long x) { if (x) longjmp(buffer, 1); }     noinline void thrower1(long x) { thrower0(x); } ...
// long f(long k) {
//     long sum = 0;
//     for (long i = 0; i < loops; i++) {
//         char* p = alloca(16 + (k & 15)); p[0] = 5;                                          (when `alloca`)
//         long before0 = opaque(k + i), ..., beforeN = opaque(k + i + N - 1);                 more of them than there are registers
//         if (_setjmp(buffer)) { sum += before0 * 1 + before1 * 2 + ... + p[0]; continue; }   read only after the second return
//         thrower(opaque(k + 100) * 1 | opaque(k + 101) * 2 | ... | 1);                       as many again, all live at once
//         return 0;
//     }
//     return sum;
// }
function moduleOf({ N, vectors = false, loops = 1, depth = 0, alloca = false }) {
    const opaque = 0, thrower = 1 + depth, f = 2 + depth;
    const sigs = [{ ret: T.i64, params: [T.i64] }, { ret: T.i32, params: [T.i64] }, { ret: T.void, params: [T.i64, T.i32] }, { ret: T.void, params: [T.i64] }];
    const SUM = N, INDEX = N + 1, POINTER = N + 2;
    const entry = new Block(1);
    entry.run("LocalSet", SUM, entry.def("ConstI64", s(0)));
    entry.run("LocalSet", INDEX, entry.def("ConstI64", s(0)));
    entry.run("Jump", 1);
    const first = new Block(entry.next);
    const base = first.def("Add", 0, first.def("LocalGet", INDEX));
    if (alloca) {
        const pointer = first.def("StackAlloc", first.def("Add", first.def("And", 0, first.def("ConstI64", s(15))), first.def("ConstI64", s(16))), 16);
        first.run("Store", b(MEM.i8u), first.def("ConstI32", s(5)), pointer, s(0));
        first.run("LocalSet", POINTER, pointer);
    }
    for (let i = 0; i < N; i++) {
        let value = first.def("Call", opaque, 1, first.def("Add", base, first.def("ConstI64", s(i))));
        if (vectors)
            value = first.def("VSplat", b(LANE.i64x2), value);
        first.run("LocalSet", i, value);
    }
    first.run("Br", first.def("CallExtern", 0, 1, first.def("DataAddr", 0)), 3, 2);
    const onward = new Block(first.next);
    const products = [];
    for (let i = 0; i < N; i++)
        products.push(onward.def("Call", opaque, 1, onward.def("Add", 0, onward.def("ConstI64", s(100 + i)))));
    let sum = onward.def("ConstI64", s(1));
    products.forEach((product, i) => { sum = onward.def("Or", sum, onward.def("Mul", product, onward.def("ConstI64", s(i + 1)))); });
    onward.run("Call", thrower, 1, sum);
    onward.run("Ret", onward.def("ConstI64", s(0)));
    const again = new Block(onward.next);
    let total = again.def("LocalGet", SUM);
    for (let i = 0; i < N; i++) {
        let value = again.def("LocalGet", i);
        if (vectors)
            value = again.def("Add", again.def("VExtract", b(LANE.i64x2), b(0), b(0), value), again.def("VExtract", b(LANE.i64x2), b(0), b(1), value));
        total = again.def("Add", total, again.def("Mul", value, again.def("ConstI64", s(i + 1))));
    }
    if (alloca)
        total = again.def("Add", total, again.def("ZExt32", again.def("Load", b(MEM.i8u), again.def("LocalGet", POINTER), s(0))));
    again.run("LocalSet", SUM, total);
    const next = again.def("Add", again.def("LocalGet", INDEX), again.def("ConstI64", s(1)));
    again.run("LocalSet", INDEX, next);
    again.run("Br", again.def("Lt", next, again.def("ConstI64", s(loops))), 1, 4);
    const done = new Block(again.next);
    done.run("Ret", done.def("LocalGet", SUM));
    const throwers = [{ name: "thrower0", sig: 3, noinline: true, blocks: [
        [["ConstI64", s(0)], ["Ne", 0, 1], ["Br", 2, 1, 2]],
        [["DataAddr", 0], ["ConstI32", s(1)], ["CallExtern", 1, 2, 3, 4], ["RetVoid"]],
        [["RetVoid"]]] }];
    for (let level = 1; level <= depth; level++)
        throwers.push({ name: "thrower" + level, sig: 3, noinline: true, blocks: [[["Call", level, 1, 0], ["RetVoid"]]] });
    return assemble({
        sigs,
        externs: [{ name: "_setjmp", sig: 1 }, { name: "longjmp", sig: 2 }],
        data: { size: 1024, align: 16, init: [], relocs: [] },
        funcs: [
            { name: "opaque", sig: 0, noinline: true, blocks: [[["ConstI64", s(7)], ["Mul", 0, 1], ["ConstI64", s(1)], ["Add", 2, 3], ["Ret", 4]]] },
            ...throwers,
            { name: "f", sig: 0, exported: true, returnsTwice: true, locals: [...new Array(N).fill(vectors ? T.v128 : T.i64), T.i64, T.i64, T.i64],
                blocks: [entry.insts, first.insts, onward.insts, again.insts, done.insts] },
        ],
        exports: [{ name: "f", func: f, ret: FFI.i64, args: [FFI.i64] }],
    });
}
function reference({ N, vectors = false, loops = 1, alloca = false }, k) {
    let total = 0n;
    for (let round = 0; round < loops; round++) {
        for (let i = 0; i < N; i++)
            total += ((BigInt(k) + BigInt(round) + BigInt(i)) * 7n + 1n) * BigInt(vectors ? 2 : 1) * BigInt(i + 1);
        if (alloca)
            total += 5n;
    }
    return BigInt.asIntN(64, total);
}
const shapes = [];
for (const N of [1, 4, 6, 8, 12, 20, 40]) {
    shapes.push({ N }, { N, vectors: true });
}
for (const N of [6, 12, 20]) {
    shapes.push({ N, loops: 7 }, { N, depth: 3 }, { N, alloca: true }, { N, alloca: true, loops: 5, depth: 2 }, { N, vectors: true, loops: 3, depth: 1, alloca: true });
}
for (const shape of shapes) {
    const f = $vm.cModule(moduleOf(shape)).f;
    for (const k of [0, 1, 1000, -5, 123456789]) {
        for (let again = 0; again < 3; again++)
            eq(f(k), reference(shape, k), `${JSON.stringify(shape)} kept across setjmp, k = ${k}`);
    }
}
print("setjmp keeps what was spilled ok");
