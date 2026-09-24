//@ skip if !$isFTLPlatform
//@ skip if $hostOS == "windows"
//@ requireOptions("--useDollarVM=1")

// Aggregates passed in the stack arguments (ByValStack): any number of them in one call, of any size up to the
// limit, and read from the caller's object byte for byte: an object that ends where a mapping ends is passed
// without touching what follows it.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });
const [arch, os] = $vm.cModuleHost();

// long sum(struct S a1, ..., struct S aN): the first and last 8 bytes of every argument, each weighted by its position.
// long call(const struct S* array): sum(array[0], ..., array[N - 1])
function manyAggregates(count, size, scalars = 0) {
    const callee = new Block(count + scalars);
    let sum = callee.def("ConstI64", s(0));
    for (let i = 0; i < count; i++) {
        for (const offset of [0, size - 8]) {
            const weighted = callee.def("Mul", callee.def("Load", b(MEM.i64), i, s(offset)), callee.def("ConstI64", s(i + 1)));
            sum = callee.def("Add", sum, weighted);
        }
    }
    for (let i = 0; i < scalars; i++)
        sum = callee.def("Add", sum, count + i);
    callee.run("Ret", sum);

    const caller = new Block(1);
    const args = [];
    for (let i = 0; i < count; i++)
        args.push(caller.def("Add", 0, caller.def("ConstI64", s(i * size))));
    for (let i = 0; i < scalars; i++)
        args.push(caller.def("ConstI64", s(1000 * (i + 1))));
    const result = caller.def("Call", 0, args.length, ...args);
    caller.run("Ret", result);
    return {
        sigs: [{ ret: T.i64, params: [...new Array(count).fill({ byval: size }), ...new Array(scalars).fill(T.i64)] }, { ret: T.i64, params: [T.i64] }],
        funcs: [{ name: "sum", sig: 0, noinline: true, blocks: [callee.insts] }, { name: "call", sig: 1, exported: true, blocks: [caller.insts] }],
        exports: [{ name: "call", func: 1, ret: FFI.i64, args: [FFI.ptr] }],
    };
}
for (const [count, size, scalars] of [[1, 80, 0], [12, 80, 0], [13, 80, 0], [14, 80, 0], [20, 80, 0], [13, 72, 8], [14, 65 + 7, 10], [40, 4096, 0], [3, 64, 0], [13, 64, 0], [13, 24, 7]]) {
    const words = size / 8;
    const array = new BigInt64Array(count * words);
    let expected = 0n;
    for (let i = 0; i < count; i++) {
        for (let w = 0; w < words; w++)
            array[i * words + w] = BigInt((i + 1) * 100 + w);
        expected += BigInt(i + 1) * (array[i * words] + array[i * words + words - 1]);
    }
    for (let i = 0; i < scalars; i++)
        expected += BigInt(1000 * (i + 1));
    const m = $vm.cModule(assemble(manyAggregates(count, size, scalars)));
    eq(m.call(array), expected, `${count} aggregates of ${size} bytes and ${scalars} integers`);
    for (let i = 0; i < 2000; i++)
        eq(m.call(array), expected, `${count} aggregates of ${size} bytes and ${scalars} integers, again`);
}

// int ends(struct S s) { return s.bytes[0] + 256 * s.bytes[sizeof s - 1]; }  for a struct of each size, called directly
// (its own code), inlined into the caller (a private copy), and through a function pointer.
const sizes = [1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 20, 23, 24, 31, 33, 63, 64, 65, 68, 71, 72, 79, 200, 204, 4095, 4096, 4097, 65536, 1 << 20];
function endsModule() {
    const sigs = [{ ret: T.i64, params: [T.i64] }, { ret: T.i64, params: [] }, { ret: T.void, params: [T.i64, T.i64] },
        { ret: T.i64, params: [T.i64, T.i64, T.i32, T.i32, T.i32, T.i64] }, { ret: T.i32, params: [T.i64, T.i64, T.i32] }];
    const funcs = [], exports = [];
    // void* guard(void): the address of inaccessible pages that follow two megabytes that can be read and written.
    const MAP_PRIVATE_ANONYMOUS = os === 0 ? 0x22 : 0x1002;
    funcs.push({ name: "guard", sig: 1, exported: true, blocks: [[
        ["ConstI64", s(0)], ["ConstI64", s((2 << 20) + 65536)], ["ConstI32", s(3)], ["ConstI32", s(MAP_PRIVATE_ANONYMOUS)], ["ConstI32", s(-1)],
        ["CallExtern", 0, 6, 0, 1, 2, 3, 4, 0], ["ConstI64", s(2 << 20)], ["Add", 5, 6], ["ConstI64", s(65536)], ["ConstI32", s(0)], ["CallExtern", 1, 3, 7, 8, 9], ["Ret", 7]]] });
    exports.push({ name: "guard", func: 0, ret: FFI.ptr, args: [] });
    // void put(unsigned char* end, long size): end[-size .. -1] = 0x5a, then end[-size] = 1, then end[-1] = 2
    funcs.push({ name: "put", sig: 2, exported: true, blocks: [[
        ["Sub", 0, 1], ["ConstI32", s(0x5a)], ["MemSet", 2, 3, 1], ["ConstI32", s(1)], ["Store", b(MEM.i8u), 4, 2, s(0)],
        ["ConstI32", s(2)], ["Store", b(MEM.i8u), 5, 0, s(-1)], ["RetVoid"]]] });
    exports.push({ name: "put", func: 1, ret: FFI.void, args: [FFI.ptr, FFI.i64] });
    for (const size of sizes) {
        const sig = sigs.length;
        sigs.push({ ret: T.i64, params: [{ byval: size }] });
        const ends = [[["Load", b(MEM.i8u), 0, s(0)], ["Load", b(MEM.i8u), 0, s(size - 1)], ["ConstI32", s(8)], ["Shl", 2, 3], ["Add", 1, 4], ["ZExt32", 5], ["Ret", 6]]];
        const called = funcs.length;
        funcs.push({ name: `ends${size}`, sig, noinline: true, blocks: ends });
        const inlined = funcs.length;
        funcs.push({ name: `inlinedEnds${size}`, sig, alwaysInline: true, blocks: ends });
        for (const [name, body] of [
            [`direct${size}`, [["Call", called, 1, 0], ["Ret", 1]]],
            [`inlined${size}`, [["Call", inlined, 1, 0], ["Ret", 1]]],
            [`indirect${size}`, [["FuncAddr", called], ["CallIndirect", sig, 1, 1, 0], ["Ret", 2]]],
        ]) {
            exports.push({ name, func: funcs.length, ret: FFI.i64, args: [FFI.ptr] });
            funcs.push({ name, sig: 0, exported: true, blocks: [body] });
        }
    }
    return { sigs, externs: [{ name: "mmap", sig: 3 }, { name: "mprotect", sig: 4 }], funcs, exports };
}
{
    const m = $vm.cModule(assemble(endsModule()));
    const end = m.guard();
    for (const size of sizes) {
        m.put(end, BigInt(size));
        const expected = size === 1 ? 2n + 256n * 2n : 1n + 256n * 2n;
        for (const how of ["direct", "inlined", "indirect"]) {
            for (let round = 0; round < 3; round++)
                eq(m[how + size](end - size), expected, `${size} bytes against the end of a mapping, ${how}`);
        }
    }
}
print("by-value aggregates ok");
