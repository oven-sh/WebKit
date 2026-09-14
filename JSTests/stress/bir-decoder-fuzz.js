//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// Byte-level mutations of well-formed modules: each mutant either loads or is refused with a TypeError.
// The seed and the number of mutants are fixed, so a run is the same every time. Nothing a mutant defines is
// called (no module here has a function a mutant could turn into a constructor).
load("./resources/bir-assembler.js", "caller relative");

const s = n => ({ s: n });
const b = n => ({ u8: n });
const [arch, os] = $vm.cModuleHost();
const isX86 = arch === 0, isWindows = os === 2;

const corpus = [];
function add(module) {
    const bytes = assemble(module);
    $vm.cModule(bytes); // Every seed loads.
    corpus.push(bytes);
}

// Arithmetic, locals, branches, calls, a switch.
add({
    sigs: [{ ret: T.i32, params: [T.i32, T.i32] }, { ret: T.i32, params: [T.i32] }, { ret: T.f64, params: [T.f64, T.i32] }, { ret: T.i64, params: [T.i64] }],
    funcs: [
        { name: "add", sig: 0, exported: true, blocks: [[["Add", 0, 1], ["Ret", 2]]] },
        { name: "fib", sig: 1, exported: true, blocks: [
            [["ConstI32", s(2)], ["Lt", 0, 1], ["Br", 2, 1, 2]],
            [["Ret", 0]],
            [["ConstI32", s(1)], ["Sub", 0, 3], ["Call", 1, 1, 4], ["ConstI32", s(2)], ["Sub", 0, 6], ["Call", 1, 1, 7], ["Add", 5, 8], ["Ret", 9]],
        ] },
        { name: "sum", sig: 1, exported: true, locals: [T.i32, T.i32], blocks: [
            [["Jump", 1]],
            [["LocalGet", 1], ["Lt", 1, 0], ["Br", 2, 2, 3]],
            [["LocalGet", 0], ["LocalGet", 1], ["Add", 3, 4], ["LocalSet", 0, 5], ["ConstI32", s(1)], ["Add", 4, 6], ["LocalSet", 1, 7], ["Jump", 1]],
            [["LocalGet", 0], ["Ret", 8]],
        ] },
        { name: "scale", sig: 2, exported: true, slots: [{ size: 8, align: 8 }], blocks: [
            [["SToF", b(T.f64), 1], ["Mul", 0, 2], ["SlotAddr", 0], ["Store", b(MEM.f64), 3, 4, s(0)], ["SlotAddr", 0], ["Load", b(MEM.f64), 5, s(0)], ["Ret", 6]],
        ] },
        { name: "pick", sig: 3, exported: true, blocks: [
            [["Switch", 0, 3, 2, s(-5), 1, s(2n ** 63n - 1n), 2]],
            [["ConstI64", s(10)], ["Ret", 1]],
            [["Clz", 0], ["Bswap", 2], ["ConstI32", s(3)], ["RotL", 3, 4], ["Ret", 5]],
            [["ConstI64", s(7)], ["MulHigh", 0, 6], ["Trunc", 0], ["Select", 8, 7, 0], ["Ret", 9]],
        ] },
    ],
    exports: [
        { name: "add", func: 0, ret: FFI.i32, args: [FFI.i32, FFI.i32] },
        { name: "fib", func: 1, ret: FFI.i32, args: [FFI.i32] },
        { name: "scale", func: 3, ret: FFI.f64, args: [FFI.f64, FFI.i32] },
    ],
});

// Data, relocations, thread-local data, externs, memory operations, alloca, frame address.
add({
    sigs: [{ ret: T.i64, params: [T.i64] }, { ret: T.i64, params: [T.i64, T.i64, T.i64] }, { ret: T.i32, params: [T.i64], variadic: true }],
    externs: [{ name: "memchr", sig: 1 }, { name: "environ", kind: 1, sig: 0 }, { name: "nothing_defines_this", kind: 0x80, sig: 0 }],
    data: { size: 64, align: 16, readOnly: 0, init: [1, 2, 3, 4, 5, 6, 7, 8], relocs: [{ offset: 8, kind: 0, index: 32, addend: 4 }, { offset: 16, kind: 1, index: 0 }, { offset: 24, kind: 2, index: 0 }] },
    tls: { size: 16, align: 8, init: [40, 0, 0, 0], relocs: [{ offset: 8, kind: 3, index: 0 }] },
    funcs: [
        { name: "memory", sig: 0, exported: true, slots: [{ size: 40, align: 32 }, { size: 3, align: 1 }], blocks: [[
            ["DataAddr", 8], ["Load", b(MEM.i64), 1, s(0)], ["TlsAddr", 0], ["Load", b(MEM.i32 | 0x80), 3, s(0)], ["SExt32", 4],
            ["SlotAddr", 0], ["ConstI64", s(40)], ["MemCopy", 6, 0, 7], ["ConstI32", s(0)], ["MemSet", 6, 8, 7],
            ["StackSave"], ["StackAlloc", 7, 64], ["Store", b(MEM.i8u), 8, 10, s(3)], ["StackRestore", 9],
            ["FrameAddress"], ["ExternAddr", 1], ["FuncAddr", 0], ["CallExtern", 0, 3, 0, 5, 7], ["Add", 2, 14], ["Ret", 15],
        ]] },
        { name: "variadic", sig: 2, slots: [{ size: 32, align: 8 }], blocks: [[["SlotAddr", 0], ["VaStart", 1], ["ConstI32", s(0)], ["Ret", 2]]] },
    ],
    exports: [{ name: "memory", func: 0, ret: FFI.u64, args: [FFI.ptr] }],
});

// Both parts of the data, some of each given, relocations in each.
add({
    sigs: [{ ret: T.i64, params: [] }],
    data: { size: 16384 + 48, align: 16, readOnly: 16384, constants: [1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 9], init: [10, 11, 12, 13, 14, 15, 16, 17, 18, 19],
        relocs: [{ offset: 8, kind: 0, index: 16384 }, { offset: 16384 + 16, kind: 0, index: 16 }, { offset: 16384 + 32, kind: 1, index: 0 }] },
    funcs: [{ name: "parts", sig: 0, exported: true, blocks: [[["DataAddr", 8], ["Load", b(MEM.i64), 0, s(0)], ["Load", b(MEM.i64), 1, s(16)], ["Load", b(MEM.i8u), 2, s(0)], ["ZExt32", 3], ["Ret", 4]]] }],
    exports: [{ name: "parts", func: 0, ret: FFI.u64, args: [] }],
});

// Vectors and atomics.
add({
    sigs: [{ ret: T.void, params: [T.i64, T.i64] }, { ret: T.v128, params: [T.v128, T.v128] }, { ret: T.i32, params: [T.i64, T.i32] }],
    funcs: [
        { name: "lanes", sig: 0, exported: true, blocks: [[
            ["Load", b(MEM.v128), 1, s(0)], ["VShuffle", 2, 2, { bytes: [12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3] }], ["Store", b(MEM.v128), 3, 0, s(0)],
            ["VMax", b(LANE.i32x4), b(1), 2, 3], ["VDiv", b(LANE.i32x4), b(1), 2, 3], ["VConvert", b(2), 4], ["VExtract", b(LANE.i16x8), b(1), b(7), 5],
            ["VSplat", b(LANE.i8x16), 7], ["VNarrow", b(LANE.i16x8), b(0), 6, 8], ["VBitmask", b(LANE.i8x16), 9], ["Store", b(MEM.i32), 10, 0, s(16)],
            ["RetVoid"],
        ]] },
        { name: "vadd", sig: 1, blocks: [[["VAdd", b(LANE.f32x4), 0, 1], ["VSqrt", b(LANE.f32x4), 2], ["Ret", 3]]] },
        { name: "atomics", sig: 2, exported: true, blocks: [[
            ["AtomicLoad", b(MEM.i32), b(0), 0], ["AtomicLoad", b(MEM.i8s), b(1), 0], ["AtomicStore", b(MEM.i32), b(2), 1, 0], ["AtomicStore", b(MEM.i16u), b(0), 1, 0],
            ["AtomicRmw", b(0), b(MEM.i32), b(4), 1, 0], ["AtomicCas", b(MEM.i32), b(4), b(0), 2, 1, 0], ["Fence", b(1)], ["Fence", b(4)],
            ["Add", 4, 5], ["Ret", 6],
        ]] },
    ],
    exports: [{ name: "lanes", func: 0, ret: FFI.void, args: [FFI.ptr, FFI.ptr] }, { name: "atomics", func: 2, ret: FFI.i32, args: [FFI.ptr, FFI.i32] }],
});

// Aggregates in the stack arguments, an indirect result, two results.
if (!isWindows) {
    add({
        sigs: [{ ret: T.i64, params: [{ byval: 20 }, T.i32] }, { ret: [T.i64, T.f64], params: [T.i64] }, { ret: T.void, params: [{ sret: true }, T.i64] }, { ret: T.i64, params: [T.i64] }],
        funcs: [
            { name: "callee", sig: 0, noinline: true, blocks: [[["Load", b(MEM.i32), 0, s(16)], ["SExt32", 2], ["Ret", 3]]] },
            { name: "pair", sig: 1, blocks: [[["SToF", b(T.f64), 0], ["Ret", 0, 1]]] },
            { name: "filled", sig: 2, blocks: [[["Store", b(MEM.i64), 1, 0, s(0)], ["RetVoid"]]] },
            { name: "caller", sig: 3, exported: true, slots: [{ size: 24, align: 8 }], blocks: [[
                ["ConstI32", s(1)], ["Call", 0, 2, 0, 1], ["Call", 1, 1, 2], ["SlotAddr", 0], ["Call", 2, 2, 5, 3], ["Ret", 3],
            ]] },
        ],
        exports: [{ name: "caller", func: 3, ret: FFI.i64, args: [FFI.ptr] }],
    });
}

// Inline assembly and cpuid.
if (isX86) {
    add({
        sigs: [{ ret: T.i64, params: [T.i64, T.i64] }, { ret: T.i32, params: [T.i32] }],
        funcs: [
            { name: "mulhi", sig: 0, exported: true, blocks: [[
                ["InlineAsm", b(0), 3, b(0x48), b(0xf7), b(0xe1), 2, 0, b(0), 1, b(1), 2, b(T.i64), b(0), b(T.i64), b(2), 1, b(6)],
                ["Ret", 3]]] },
            { name: "leaf", sig: 1, exported: true, blocks: [[["ConstI32", s(0)], ["CpuId", 0, 1], ["Ret", 3]]] },
        ],
        exports: [{ name: "mulhi", func: 0, ret: FFI.u64, args: [FFI.u64, FFI.u64] }],
    });
}

let state = 0x9e3779b9;
function random(n) { state ^= state << 13; state >>>= 0; state ^= state >>> 17; state ^= state << 5; state >>>= 0; return state % n; }
const interesting = [0, 1, 2, 3, 4, 5, 8, 16, 0x3f, 0x40, 0x7f, 0x80, 0x81, 0xfe, 0xff];

let loaded = 0, refused = 0;
for (let iteration = 0; iteration < 6000; iteration++) {
    const bytes = Array.from(corpus[random(corpus.length)]);
    for (let edits = 1 + random(3); edits--;) {
        // Past the header: a module for another target is refused before anything else is looked at.
        const position = 8 + random(bytes.length - 8);
        switch (random(6)) {
        case 0: bytes[position] = interesting[random(interesting.length)]; break;
        case 1: bytes[position] = random(256); break;
        case 2: bytes[position] ^= 1 << random(8); break;
        case 3: bytes.splice(position, 1); break;
        case 4: bytes.splice(position, 0, interesting[random(interesting.length)]); break;
        case 5: bytes[position] = (bytes[position] + 1) & 0xff; break;
        }
    }
    try {
        $vm.cModule(new Uint8Array(bytes));
        loaded++;
    } catch (error) {
        if (!(error instanceof TypeError))
            throw new Error(`mutant ${iteration}: ${error}`);
        refused++;
    }
}
if (!loaded || !refused)
    throw new Error(`the mutations are not doing their job: ${loaded} loaded, ${refused} refused`);
print("decoder fuzz ok:", loaded, "loaded,", refused, "refused");
