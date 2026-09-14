//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// The two parts of a module's data (ffi/BIR.h, "The data segment"): the constant part at 0 and the writable part
// at readOnly, each given as the bytes it starts with and no more. What neither run covers is zero, relocations
// reach into both parts, the whole pages of the constant part refuse a store, and what follows them takes one.
load("./resources/bir-assembler.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n }), t8 = n => ({ u8: n });
const [, os] = $vm.cModuleHost();
const isWindows = os === 2;
const PART = 16384; // BIR::dataPage: what the writable part starts at a multiple of

// Every module here is these functions over its own data.
function moduleWith(data) {
    const address = block => { const base = block.def("DataAddr", 0); const offset = block.def("SExt32", 0); return block.def("Add", base, offset); };
    const functions = [];
    const sigs = [
        { ret: T.i32, params: [T.i32] },
        { ret: T.void, params: [T.i32, T.i32] },
        { ret: T.i32, params: [T.i32, T.i32] },
        { ret: T.i32, params: [T.i64] },                // pipe
        { ret: T.i64, params: [T.i32, T.i64, T.i64] },  // read, write
        { ret: T.i32, params: [] },
    ];
    const define = (name, sig, ffiRet, ffiArgs, build, slots = []) => {
        const block = new Block(sigs[sig].params.length);
        build(block);
        functions.push({ name, sig, ffiRet, ffiArgs, slots, blocks: [block.insts] });
    };
    // int load8(int offset) { return data[offset]; }
    define("load8", 0, FFI.i32, [FFI.i32], b => { const p = address(b); const v = b.def("Load", t8(MEM.i8u), p, s(0)); b.run("Ret", v); });
    // void store8(int offset, int value) { data[offset] = value; }
    define("store8", 1, FFI.void, [FFI.i32, FFI.i32], b => { const p = address(b); b.run("Store", t8(MEM.i8u), 1, p, s(0)); b.run("RetVoid"); });
    // int pointsTo(int offset, int target) { return *(char **)&data[offset] == &data[target]; }
    define("pointsTo", 2, FFI.i32, [FFI.i32, FFI.i32], b => {
        const p = address(b);
        const held = b.def("Load", t8(MEM.i64), p, s(0));
        const base = b.def("DataAddr", 0);
        const target = b.def("SExt32", 1);
        const expected = b.def("Add", base, target);
        b.run("Ret", b.def("Eq", held, expected));
    });
    // int callThrough(int offset) { return (*(int (**)(void))&data[offset])(); }
    define("callThrough", 0, FFI.i32, [FFI.i32], b => { const p = address(b); const f = b.def("Load", t8(MEM.i64), p, s(0)); b.run("Ret", b.def("CallIndirect", 5, f, 0)); });
    define("seventySeven", 5, FFI.i32, [], b => { b.run("Ret", b.def("ConstI32", s(77))); });
    if (!isWindows) {
        // int probe(int offset) { int fds[2]; char byte = 7; if (pipe(fds)) return -2; write(fds[1], &byte, 1);
        //                         long result = read(fds[0], &data[offset], 1); close(fds[0]); close(fds[1]); return result; }
        // The system writes the byte, so where the page cannot be written it says so (-1, EFAULT) and nothing faults.
        define("probe", 0, FFI.i32, [FFI.i32], b => {
            const p = address(b);
            const fds = b.def("SlotAddr", 0);
            const failed = b.def("CallExtern", 0, 1, fds);
            const byte = b.def("SlotAddr", 1);
            const seven = b.def("ConstI32", s(7));
            b.run("Store", t8(MEM.i8u), seven, byte, s(0));
            const readEnd = b.def("Load", t8(MEM.i32), fds, s(0));
            const writeEnd = b.def("Load", t8(MEM.i32), fds, s(4));
            const one = b.def("ConstI64", s(1));
            b.def("CallExtern", 1, 3, writeEnd, byte, one);
            const result = b.def("CallExtern", 2, 3, readEnd, p, one);
            b.def("CallExtern", 3, 1, readEnd);
            b.def("CallExtern", 3, 1, writeEnd);
            const narrow = b.def("Trunc", result);
            const minusTwo = b.def("ConstI32", s(-2));
            b.run("Ret", b.def("Select", failed, minusTwo, narrow));
        }, [{ size: 8, align: 4 }, { size: 1, align: 1 }]);
    }
    return $vm.cModule(assemble({
        sigs,
        externs: isWindows ? [] : [{ name: "pipe", sig: 3 }, { name: "write", sig: 4 }, { name: "read", sig: 4 }, { name: "close", sig: 0 }],
        data: { align: 16, constants: [], init: [], relocs: [], ...data },
        funcs: functions.map(f => ({ name: f.name, sig: f.sig, exported: true, slots: f.slots, blocks: f.blocks })),
        exports: functions.map((f, index) => ({ name: f.name, func: index, ret: f.ffiRet, args: f.ffiArgs })),
    }));
}
const FUNC_SEVENTY_SEVEN = 4;
const refusesAStore = (m, offset, what) => { if (!isWindows) eq(m.probe(offset), -1, `${what}: a store to offset ${offset} is refused`); };
const takesAStore = (m, offset, what) => { if (!isWindows) { eq(m.probe(offset), 1, `${what}: a store to offset ${offset}`); eq(m.load8(offset), 7, `${what}: what was stored at ${offset}`); } };

// Constants and nothing else. With no writable part the constant part ends wherever it ends.
{
    const what = "constants only";
    const m = moduleWith({ size: PART, readOnly: PART, constants: [41, 42, 43], relocs: [{ offset: 8, kind: 0, index: 1, addend: 1 }, { offset: 16, kind: 1, index: FUNC_SEVENTY_SEVEN }] });
    eq(m.load8(0), 41, `${what}: first byte`);
    eq(m.load8(2), 43, `${what}: last byte given`);
    eq(m.load8(3), 0, `${what}: the byte after the last one given`);
    eq(m.load8(PART - 1), 0, `${what}: last byte`);
    eq(m.pointsTo(8, 2), 1, `${what}: a relocation in the constant part`);
    eq(m.callThrough(16), 77, `${what}: a function's address in the constant part`);
    refusesAStore(m, 0, what);
    refusesAStore(m, 8, what);
    refusesAStore(m, PART - 1, what);

    const small = moduleWith({ size: 100, readOnly: 100, constants: new Array(100).fill(9) });
    eq(small.load8(0) + small.load8(99), 18, "a hundred constant bytes");
    const ragged = moduleWith({ size: PART + 100, readOnly: PART + 100, constants: [1] });
    eq(ragged.load8(0), 1, "constants that end in the middle of a page");
    eq(ragged.load8(PART + 99), 0, "constants that end in the middle of a page: last byte");
    refusesAStore(ragged, 1, "constants that end in the middle of a page");
}

// Writable data and nothing else.
{
    const what = "writable only";
    const m = moduleWith({ size: 64, readOnly: 0, init: [1, 2, 3], relocs: [{ offset: 8, kind: 0, index: 64 }, { offset: 16, kind: 1, index: FUNC_SEVENTY_SEVEN }] });
    eq(m.load8(0), 1, `${what}: first byte`);
    eq(m.load8(2), 3, `${what}: last byte given`);
    eq(m.load8(3), 0, `${what}: the byte after the last one given`);
    eq(m.load8(63), 0, `${what}: last byte`);
    eq(m.pointsTo(8, 64), 1, `${what}: a relocation to the end of the data`);
    eq(m.callThrough(16), 77, `${what}: a function's address`);
    m.store8(0, 200);
    eq(m.load8(0), 200, `${what}: a store`);
    takesAStore(m, 63, what);
}

// Both. Neither run holds the bytes between the end of the constants and the start of the writable part.
{
    const what = "both parts";
    const m = moduleWith({
        size: PART + 64, readOnly: PART, constants: [41, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1], init: [1, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2],
        relocs: [
            { offset: 8, kind: 0, index: PART },                        // the constant part points into the writable part
            { offset: PART + 8, kind: 0, index: 0, addend: 4 },         // and the writable part into the constant part
            { offset: 24, kind: 1, index: FUNC_SEVENTY_SEVEN },         // past the constants given, in the constant part
            { offset: PART + 32, kind: 1, index: FUNC_SEVENTY_SEVEN },  // past the bytes given, in the writable part
        ],
    });
    eq(m.load8(0), 41, `${what}: the constant part`);
    eq(m.load8(PART), 1, `${what}: the writable part`);
    eq(m.load8(16), 0, `${what}: after the constants given`);
    eq(m.load8(PART - 1), 0, `${what}: the last byte of the constant part`);
    eq(m.load8(PART + 16), 0, `${what}: after the writable bytes given`);
    eq(m.load8(PART + 63), 0, `${what}: the last byte`);
    eq(m.pointsTo(8, PART), 1, `${what}: a relocation in the constant part that replaces bytes given`);
    eq(m.pointsTo(PART + 8, 4), 1, `${what}: a relocation in the writable part that replaces bytes given`);
    eq(m.callThrough(24), 77, `${what}: a relocation in the constant part past the bytes given`);
    eq(m.callThrough(PART + 32), 77, `${what}: a relocation in the writable part past the bytes given`);
    m.store8(PART, 5);
    eq(m.load8(PART), 5, `${what}: a store to the writable part`);
    m.store8(PART + 63, 6);
    eq(m.load8(PART + 63), 6, `${what}: a store to the last byte`);
    refusesAStore(m, 0, what);
    refusesAStore(m, 100, what);
    refusesAStore(m, PART - 1, what);
    takesAStore(m, PART, what);
    takesAStore(m, PART + 40, what);
    eq(m.load8(0), 41, `${what}: the constant part afterwards`);
}

// Both parts and no bytes given for either, or for one; runs that fill their parts; no data at all.
{
    const empty = moduleWith({ size: PART + 8, readOnly: PART });
    eq(empty.load8(0) + empty.load8(PART - 1) + empty.load8(PART) + empty.load8(PART + 7), 0, "two empty runs");
    refusesAStore(empty, 0, "two empty runs");
    takesAStore(empty, PART + 7, "two empty runs");
    const noConstants = moduleWith({ size: 2 * PART + 8, readOnly: 2 * PART, init: [3] });
    eq(noConstants.load8(2 * PART), 3, "an empty constant run");
    eq(noConstants.load8(PART), 0, "an empty constant run: the constant part");
    refusesAStore(noConstants, PART, "an empty constant run");
    const noWritable = moduleWith({ size: PART + 8, readOnly: PART, constants: [4] });
    eq(noWritable.load8(0), 4, "an empty writable run");
    eq(noWritable.load8(PART), 0, "an empty writable run: the writable part");
    takesAStore(noWritable, PART, "an empty writable run");
    const full = moduleWith({ size: PART + 8, readOnly: PART, constants: new Array(PART).fill(8), init: new Array(8).fill(9) });
    eq(full.load8(PART - 1), 8, "runs that fill their parts: the last constant");
    eq(full.load8(PART + 7), 9, "runs that fill their parts: the last byte");
    moduleWith({ size: 0, readOnly: 0 });
}

// What does not fit its part, and a writable part that would share a page with the constants.
{
    const refused = (what, data, pattern) => {
        let error = null;
        try { moduleWith(data); } catch (e) { error = e; }
        if (!(error instanceof TypeError) || !pattern.test(error.message))
            throw new Error(`${what}: ${error ? error : "was accepted"}`);
    };
    refused("constants that run into the writable part", { size: PART + 8, readOnly: PART, constants: new Array(PART + 1).fill(1) }, /more constant data than the constant part/);
    refused("one constant byte and no constant part", { size: 8, readOnly: 0, constants: [1] }, /more constant data than the constant part/);
    refused("writable bytes that run past the end", { size: PART + 8, readOnly: PART, init: new Array(9).fill(1) }, /more initialized data than the writable part/);
    refused("writable bytes and no writable part", { size: PART, readOnly: PART, init: [1] }, /more initialized data than the writable part/);
    refused("a writable run after 100 constant bytes", { size: 200, readOnly: 100, init: [1] }, /does not start at a multiple of 16384/);
    refused("a zero-filled writable part after 100 constant bytes", { size: 200, readOnly: 100 }, /does not start at a multiple of 16384/);
    refused("a writable part at 4096", { size: 8192, readOnly: 4096, init: [1] }, /does not start at a multiple of 16384/);
    refused("a writable part one byte after 16384", { size: PART + 8, readOnly: PART + 1, init: [1] }, /does not start at a multiple of 16384/);
    refused("a constant part longer than the data", { size: PART, readOnly: 2 * PART }, /bad data segment size/);
}

print("rodata ok");
