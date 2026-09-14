//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// BIR bytes are input like any other: a module that breaks a rule of the format (ffi/BIR.h) is refused with
// a TypeError naming the rule, and never reaches the code generator. One case or more for each rule, with the
// value on the allowed side of each limit next to it.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });
const [arch, os] = $vm.cModuleHost();
const isX86 = arch === 0, isWindows = os === 2;

let cases = 0;
function bytesOf(module) { return module instanceof Uint8Array ? module : assemble(module); }
// `by` is "BIR: " for what the decoder refuses, "" for what the loader does.
function rejects(what, module, pattern, by = "BIR: ") {
    cases++;
    let error = null;
    try { $vm.cModule(bytesOf(module)); } catch (e) { error = e; }
    if (!error)
        throw new Error(`${what}: was accepted`);
    if (!(error instanceof TypeError) || !String(error.message).startsWith(by))
        throw new Error(`${what}: refused with ${error}`);
    if (!pattern.test(error.message))
        throw new Error(`${what}: refused with "${error.message}", expected ${pattern}`);
}
function accepts(what, module) {
    cases++;
    try { return $vm.cModule(bytesOf(module)); } catch (e) { throw new Error(`${what}: refused with ${e}`); }
}

const intToInt = { ret: T.i32, params: [T.i32] };
const identity = (name = "f", more = {}) => ({ name, sig: 0, exported: true, blocks: [[["Ret", 0]]], ...more });
const exportOf = (name, func = 0) => ({ name, func, ret: FFI.i32, args: [FFI.i32] });
// int f(int x) { return x; }, and `change` applied to the description.
function variant(change) {
    const module = { sigs: [intToInt], funcs: [identity()], exports: [exportOf("f")] };
    change(module);
    return module;
}
// One function, `sig` and `blocks` given, not exported to JavaScript: for what only has to decode.
const body = (sig, blocks, more = {}) => ({ sigs: [sig], funcs: [{ name: "f", sig: 0, exported: true, blocks, ...more }], ...more.module });
const voidFunction = (blocks, more) => body({ ret: T.void, params: [] }, blocks, more);

// Header.
{
    const good = assemble(variant(() => { }));
    eq(accepts("the unchanged module", good).f(7), 7, "the unchanged module runs");
    const patched = (offset, value) => { const copy = new Uint8Array(good); copy[offset] = value; return copy; };
    rejects("magic", patched(3, 0x31), /bad magic/);
    rejects("arch 2", patched(4, 2), /unsupported target/);
    rejects("os 4", patched(5, 4), /unsupported target/);
    rejects("4-byte pointers", patched(6, 4), /unsupported target/);
    rejects("reserved byte", patched(7, 1), /reserved header byte/);
    rejects("trailing byte", new Uint8Array([...good, 0]), /trailing bytes/);
    rejects("empty input", new Uint8Array(0), /unexpected end of input/);
    // Cut short anywhere, it is refused; changed anywhere, it is refused or it loads.
    for (let length = 0; length < good.length; length++)
        rejects(`the first ${length} bytes`, good.slice(0, length), /./);
    for (let offset = 8; offset < good.length; offset++) {
        for (const value of [0, 1, 2, 0x7f, 0x80, 0xff, good[offset] ^ 1, good[offset] ^ 0x80, (good[offset] + 1) & 0xff]) {
            cases++;
            try { $vm.cModule(patched(offset, value)); } catch (e) {
                if (!(e instanceof TypeError))
                    throw new Error(`byte ${offset} = ${value}: ${e}`);
            }
        }
    }
}

// Signatures.
{
    const returning = (ret, values) => body({ ret, params: [] }, [[...values, ["Ret", ...values.map((_, i) => i)]]]);
    const i64 = ["ConstI64", s(1)], i32 = ["ConstI32", s(1)], f64 = ["ConstF64", { f64: 1 }];
    const f32 = ["ConstF32", { bytes: [0, 0, 128, 63] }], v128 = ["ConstV128", { bytes: new Array(16).fill(1) }];
    rejects("five results", returning([T.i32, T.i32, T.f64, T.f64, T.f64], [i32, i32, f64, f64, f64]), /too many results/);
    rejects("three integer results", returning([T.i64, T.i64, T.i64], [i64, i64, i64]), /more results than the target has result registers/);
    rejects("four i32 results", returning([T.i32, T.i32, T.i32, T.i32], [i32, i32, i32, i32]), /more results than the target has result registers/);
    if (isWindows)
        rejects("two results on Win64", returning([T.i64, T.i64], [i64, i64]), /more results than the target has result registers/);
    else {
        accepts("two integer results", returning([T.i64, T.i32], [i64, i32]));
        accepts("two integer and two floating-point results", returning([T.i64, T.i64, T.f64, T.f64], [i64, i64, f64, f64]));
        if (isX86) {
            rejects("three double results", returning([T.f64, T.f64, T.f64], [f64, f64, f64]), /more results than the target has result registers/);
            rejects("three vector results", returning([T.v128, T.v128, T.v128], [v128, v128, v128]), /more results than the target has result registers/);
            rejects("a float, a double and a vector", returning([T.f32, T.f64, T.v128], [f32, f64, v128]), /more results than the target has result registers/);
        } else
            accepts("four float results", returning([T.f32, T.f32, T.f32, T.f32], [f32, f32, f32, f32]));
    }
    rejects("a void result", returning([T.void], [i32]), /bad type/);
    rejects("result type 6", returning([6], [i32]), /bad type/);
    rejects("signature flags 2", voidFunction([[["RetVoid"]]], { module: { sigs: [{ ret: T.void, params: [], variadic: 2 }] } }), /unknown signature flags/);

    const taking = params => body({ ret: T.void, params }, [[["RetVoid"]]]);
    rejects("a void parameter", taking([T.void]), /bad type/);
    rejects("parameter kind 3", taking([{ kind: 3 }]), /bad parameter kind/);
    if (isWindows) {
        rejects("an aggregate in the stack arguments on Win64", taking([{ byval: 24 }]), /cannot be passed in the stack arguments/);
        rejects("a vector by value on Win64", taking([T.v128]), /vector cannot be passed by value/);
    } else {
        for (const size of [1, 3, 17, 20, 24, 65, 1 << 20])
            accepts(`a by-value argument of ${size} bytes`, taking([{ byval: size }]));
        rejects("a by-value argument of no bytes", taking([{ byval: 0 }]), /bad by-value argument size/);
        rejects("a by-value argument over a megabyte", taking([{ byval: (1 << 20) + 1 }]), /bad by-value argument size/);
        for (const align of [1, 4, 12, 32, 4096])
            rejects(`a by-value argument aligned to ${align}`, taking([{ byval: 32, align }]), /bad by-value argument alignment/);
        accepts("a by-value argument aligned to 16", taking([{ byval: 32, align: 16 }]));
        rejects("exhausts 3", taking([{ byval: 32, exhausts: 3 }]), /bad exhausts/);
        accepts("1023 by-value arguments of a megabyte", taking(new Array(1023).fill({ byval: 1 << 20 })));
        rejects("1025 by-value arguments of a megabyte", taking(new Array(1025).fill({ byval: 1 << 20 })), /by-value arguments are too large/);
    }
    accepts("an indirect result first", taking([{ sret: true }, T.i32]));
    rejects("an indirect result second", taking([T.i32, { sret: true }]), /indirect result must be the first/);
    rejects("two indirect results", taking([{ sret: true }, { sret: true }]), /indirect result must be the first/);
}

// Externs, data, thread-local data, relocations.
{
    const withExtern = entry => voidFunction([[["RetVoid"]]], { module: { externs: [entry] } });
    rejects("extern kind 2", withExtern({ name: "x", kind: 2, sig: 0 }), /bad extern kind/);
    rejects("extern signature out of range", withExtern({ name: "x", sig: 1 }), /extern signature out of range/);
    rejects("data extern with a signature", withExtern({ name: "environ", kind: 1, sig: 5 }), /a data extern has no signature/);
    accepts("weak data extern nothing defines", withExtern({ name: "nothing_defines_this", kind: 0x81, sig: 0 }));
    rejects("extern nothing defines", withExtern({ name: "nothing_defines_this", kind: 1, sig: 0 }), /^undefined symbol 'nothing_defines_this'$/, "");

    const withData = data => voidFunction([[["RetVoid"]]], { module: { data: { align: 8, init: [], relocs: [], ...data } } });
    rejects("more initialized data than data", withData({ size: 2, init: [1, 2, 3] }), /bad data segment size/);
    rejects("more constant data than data", withData({ size: 8, readOnly: 16 }), /bad data segment size/);
    rejects("data over 4 GiB", withData({ size: 2 ** 32 + 1 }), /bad data segment size/);
    for (const align of [0, 3, 8192])
        rejects(`data aligned to ${align}`, withData({ size: 8, align }), /bad data segment alignment/);
    accepts("data aligned to 4096", withData({ size: 8, align: 4096 }));
    rejects("relocation kind 3 in data", withData({ size: 8, relocs: [{ offset: 0, kind: 3, index: 0 }] }), /bad reloc kind/);
    rejects("relocation kind 4", withData({ size: 8, relocs: [{ offset: 0, kind: 4, index: 0 }] }), /bad reloc kind/);
    rejects("relocation that does not fit", withData({ size: 8, relocs: [{ offset: 1, kind: 0, index: 0 }] }), /reloc offset out of range/);
    rejects("relocation past the data", withData({ size: 8, relocs: [{ offset: 9, kind: 0, index: 0 }] }), /reloc offset out of range/);
    accepts("relocation to the end of the data", withData({ size: 8, relocs: [{ offset: 0, kind: 0, index: 8 }] }));
    rejects("relocation to past the end of the data", withData({ size: 8, relocs: [{ offset: 0, kind: 0, index: 9 }] }), /reloc index out of range/);
    rejects("relocation to function 1 of 1", withData({ size: 8, relocs: [{ offset: 0, kind: 1, index: 1 }] }), /reloc index out of range/);
    rejects("relocation to extern 0 of 0", withData({ size: 8, relocs: [{ offset: 0, kind: 2, index: 0 }] }), /reloc index out of range/);

    const withTls = tls => voidFunction([[["RetVoid"]]], { module: { tls: { align: 8, init: [], ...tls } } });
    rejects("thread-local data over 256 MiB", withTls({ size: 2 ** 28 + 1 }), /bad thread-local segment size/);
    rejects("more initialized thread-local data than there is", withTls({ size: 1, init: [1, 2] }), /bad thread-local segment size/);
    for (const align of [0, 6, 8192])
        rejects(`thread-local data aligned to ${align}`, withTls({ size: 8, align }), /bad thread-local segment alignment/);
    accepts("thread-local relocation to itself", withTls({ size: 16, relocs: [{ offset: 8, kind: 3, index: 0 }] }));
    rejects("thread-local relocation past itself", withTls({ size: 16, relocs: [{ offset: 8, kind: 3, index: 17 }] }), /reloc index out of range/);
}

// Function declarations and names.
{
    rejects("function signature out of range", variant(m => { m.funcs[0].sig = 1; }), /function signature out of range/);
    rejects("function flags 0x20", variant(m => { m.funcs[0].flags = 0x21; }), /unknown function flags/);
    accepts("every function flag there is", variant(m => { m.funcs[0].flags = 0x1f; }));
    for (const [what, name] of [["0xff", "\xff"], ["a lone continuation byte", "a\x80"], ["an overlong encoding", "\xc0\xaf"], ["a cut-off sequence", "\xe2\x82"], ["a surrogate", "\xed\xa0\x80"]]) {
        rejects(`function name that is ${what}`, variant(m => { m.funcs[0].name = name; }), /function name is not UTF-8/);
        rejects(`export name that is ${what}`, variant(m => { m.exports[0].name = name; }), /export name is not UTF-8/);
    }
    accepts("UTF-8 names", variant(m => { m.funcs[0].name = m.exports[0].name = "\xc3\xa9t\xc3\xa9"; }));
}

// Bodies: locals, slots, blocks.
{
    rejects("local of type void", voidFunction([[["RetVoid"]]], { locals: [T.void] }), /bad type/);
    for (const [what, slot] of [["aligned to 0", { size: 8, align: 0 }], ["aligned to 3", { size: 8, align: 3 }], ["aligned to 8192", { size: 8, align: 8192 }], ["over 256 MiB", { size: 2 ** 28 + 1, align: 8 }]])
        rejects(`slot ${what}`, voidFunction([[["RetVoid"]]], { slots: [slot] }), /bad stack slot/);
    accepts("slot of 256 MiB aligned to 4096", voidFunction([[["RetVoid"]]], { slots: [{ size: 2 ** 28, align: 4096 }] }));
    accepts("slot of no bytes", voidFunction([[["RetVoid"]]], { slots: [{ size: 0, align: 1 }] }));
    accepts("three slots of 256 MiB", voidFunction([[["RetVoid"]]], { slots: new Array(3).fill({ size: 2 ** 28, align: 16 }) }));
    rejects("five slots of 256 MiB", voidFunction([[["RetVoid"]]], { slots: new Array(5).fill({ size: 2 ** 28, align: 16 }) }), /stack frame is too large/);
    rejects("seventeen slots of 256 MiB", voidFunction([[["RetVoid"]]], { slots: new Array(17).fill({ size: 2 ** 28, align: 16 }) }), /stack frame is too large/);
    accepts("a thousand slots of a megabyte", voidFunction([[["RetVoid"]]], { slots: new Array(1000).fill({ size: 1 << 20, align: 16 }) }));
    rejects("1025 slots of a megabyte", voidFunction([[["RetVoid"]]], { slots: new Array(1025).fill({ size: 1 << 20, align: 16 }) }), /stack frame is too large/);

    rejects("no blocks", voidFunction([]), /function has no blocks/);
    rejects("an empty block", voidFunction([[]]), /empty block/);
    rejects("no terminator", voidFunction([[["ConstI32", s(1)]]]), /block must end in exactly one terminator/);
    rejects("a terminator in the middle", voidFunction([[["RetVoid"], ["RetVoid"]]]), /block must end in exactly one terminator/);
    rejects("a jump to the block after the last", voidFunction([[["Jump", 1]]]), /block out of range/);
    accepts("a jump to itself", voidFunction([[["Jump", 0]]]));
    rejects("opcode 0", voidFunction([[[0], ["RetVoid"]]]), /unknown opcode 0/);
    rejects("opcode 0xff", voidFunction([[[0xff], ["RetVoid"]]]), /unknown opcode 255/);
    // A count the input cannot hold is refused before anything that size is allocated.
    {
        const good = assemble(voidFunction([[["RetVoid"]]]));
        // ... nlocals nslots nblocks=1 ninsts=1 RetVoid nexports nlibraries nconstructors ndestructors
        const blockCount = good.length - 7;
        eq(good[blockCount], 1, "found the block count");
        const huge = new Uint8Array([...good.slice(0, blockCount), 0x80, 0x80, 0x80, 0x08, ...good.slice(blockCount + 1)]);
        rejects("sixteen million blocks in forty bytes", huge, /unexpected end of input/);
    }
}

// Values and their types.
{
    const intFunction = blocks => body(intToInt, blocks);
    rejects("use of a value not yet defined", intFunction([[["Add", 0, 1], ["Ret", 1]]]), /use of an undefined value/);
    rejects("use of a value from another block", intFunction([[["ConstI32", s(1)], ["Jump", 1]], [["Ret", 1]]]), /value used outside its defining block/);
    accepts("use of a parameter in another block", intFunction([[["Jump", 1]], [["Ret", 0]]]));
    rejects("i32 + i64", intFunction([[["ConstI64", s(1)], ["Add", 0, 1], ["Ret", 0]]]), /binary operands differ in type/);
    rejects("returning an i64 as an i32", intFunction([[["ConstI64", s(1)], ["Ret", 1]]]), /operand has the wrong type/);
    rejects("RetVoid from an int function", intFunction([[["RetVoid"]]]), /RetVoid in a non-void function/);
    rejects("Ret from a void function", voidFunction([[["ConstI32", s(1)], ["Ret", 0]]]), /Ret in a void function/);
    accepts("ConstI32 of INT32_MAX and INT32_MIN", intFunction([[["ConstI32", s(2 ** 31 - 1)], ["ConstI32", s(-(2 ** 31))], ["Ret", 1]]]));
    rejects("ConstI32 of INT32_MAX + 1", intFunction([[["ConstI32", s(2 ** 31)], ["Ret", 1]]]), /ConstI32 out of range/);
    rejects("ConstI32 of INT32_MIN - 1", intFunction([[["ConstI32", s(-(2 ** 31) - 1)], ["Ret", 1]]]), /ConstI32 out of range/);
    rejects("UDiv of doubles", voidFunction([[["ConstF64", { f64: 1 }], ["UDiv", 0, 0], ["RetVoid"]]]), /integer operation on a float/);
    rejects("shift of a double", voidFunction([[["ConstF64", { f64: 1 }], ["ConstI32", s(1)], ["Shl", 0, 1], ["RetVoid"]]]), /shift of a float/);
    rejects("unsigned compare of doubles", voidFunction([[["ConstF64", { f64: 1 }], ["ULt", 0, 0], ["RetVoid"]]]), /unsigned compare of a float/);
    rejects("i32 to i32 bitcast", intFunction([[["Bitcast", b(T.i32), 0], ["Ret", 1]]]), /bad bitcast/);
    rejects("select with arms of two types", intFunction([[["ConstI64", s(1)], ["Select", 0, 0, 1], ["Ret", 0]]]), /select arms differ in type/);
    rejects("local out of range", intFunction([[["LocalGet", 0], ["Ret", 0]]]), /local out of range/);
    rejects("local set to another type", body(intToInt, [[["ConstI64", s(1)], ["LocalSet", 0, 1], ["Ret", 0]]], { locals: [T.i32] }), /operand has the wrong type/);

    // Memory.
    const pointerFunction = blocks => body({ ret: T.void, params: [T.i64] }, blocks);
    accepts("load at offsets INT32_MAX and INT32_MIN", pointerFunction([[["Load", b(MEM.i32), 0, s(2 ** 31 - 1)], ["Load", b(MEM.i32), 0, s(-(2 ** 31))], ["RetVoid"]]]));
    rejects("load at offset INT32_MAX + 1", pointerFunction([[["Load", b(MEM.i32), 0, s(2 ** 31)], ["RetVoid"]]]), /memory offset out of range/);
    rejects("store at offset INT32_MIN - 1", pointerFunction([[["ConstI32", s(1)], ["Store", b(MEM.i32), 1, 0, s(-(2 ** 31) - 1)], ["RetVoid"]]]), /memory offset out of range/);
    rejects("load of kind 9", pointerFunction([[["Load", b(9), 0, s(0)], ["RetVoid"]]]), /bad memory kind/);
    rejects("store of kind I8S", pointerFunction([[["ConstI32", s(1)], ["Store", b(MEM.i8s), 1, 0, s(0)], ["RetVoid"]]]), /bad memory kind/);
    rejects("store of an i64 as an i32", pointerFunction([[["Store", b(MEM.i32), 0, 0, s(0)], ["RetVoid"]]]), /stored value has the wrong type/);
    rejects("load through an i32", intFunction([[["Load", b(MEM.i32), 0, s(0)], ["Ret", 1]]]), /operand has the wrong type/);
    rejects("slot out of range", voidFunction([[["SlotAddr", 0], ["RetVoid"]]]), /slot out of range/);
    rejects("address past the data", voidFunction([[["DataAddr", 9], ["RetVoid"]]], { module: { data: { size: 8, align: 8, init: [], relocs: [] } } }), /data offset out of range/);
    accepts("address of the end of the data", voidFunction([[["DataAddr", 8], ["RetVoid"]]], { module: { data: { size: 8, align: 8, init: [], relocs: [] } } }));
    rejects("address past the thread-local data", voidFunction([[["TlsAddr", 9], ["RetVoid"]]], { module: { tls: { size: 8, align: 8, init: [] } } }), /thread-local offset out of range/);
    rejects("address of function 1 of 1", voidFunction([[["FuncAddr", 1], ["RetVoid"]]]), /function out of range/);
    rejects("address of extern 0 of 0", voidFunction([[["ExternAddr", 0], ["RetVoid"]]]), /extern out of range/);
    for (const align of [0, 24, 8192])
        rejects(`StackAlloc aligned to ${align}`, voidFunction([[["ConstI64", s(8)], ["StackAlloc", 0, align], ["RetVoid"]]]), /bad StackAlloc alignment/);
    rejects("VaStart in a function that is not variadic", pointerFunction([[["VaStart", 0], ["RetVoid"]]]), /VaStart in a function that is not variadic/);

    // Calls.
    const caller = (blocks, more) => ({ sigs: [intToInt, { ret: T.void, params: [] }], funcs: [identity("callee", { exported: false }), { name: "f", sig: 1, exported: true, blocks }], ...more });
    rejects("call of function 2 of 2", caller([[["Call", 2, 0], ["RetVoid"]]]), /function out of range/);
    rejects("call with too few arguments", caller([[["Call", 0, 0], ["RetVoid"]]]), /wrong number of arguments/);
    rejects("call with too many arguments", caller([[["ConstI32", s(1)], ["Call", 0, 2, 0, 0], ["RetVoid"]]]), /wrong number of arguments/);
    rejects("call with an argument of the wrong type", caller([[["ConstI64", s(1)], ["Call", 0, 1, 0], ["RetVoid"]]]), /call argument has the wrong type/);
    rejects("indirect call through signature 2 of 2", caller([[["ConstI64", s(1)], ["CallIndirect", 2, 0, 0], ["RetVoid"]]]), /signature out of range/);
    rejects("indirect call through an i32", caller([[["ConstI32", s(1)], ["CallIndirect", 1, 0, 0], ["RetVoid"]]]), /operand has the wrong type/);
    rejects("call of extern 0 of 0", caller([[["CallExtern", 0, 0], ["RetVoid"]]]), /extern out of range/);
    rejects("call of a data extern", caller([[["CallExtern", 0, 0], ["RetVoid"]]], { externs: [{ name: "environ", kind: 1, sig: 0 }] }), /call of a data extern/);

    // Switch.
    const switchOn = (type, cases) => body({ ret: T.void, params: [type] }, [[["Switch", 0, 1, cases.length, ...cases.flatMap(value => [s(value), 1])]], [["RetVoid"]]]);
    accepts("switch on the extreme 64-bit values", switchOn(T.i64, [2n ** 63n - 1n, 2n ** 63n - 2n, -(2n ** 63n), -(2n ** 63n) + 1n, -1n, 0n]));
    accepts("switch on the extreme 32-bit values", switchOn(T.i32, [2 ** 31 - 1, -(2 ** 31), -1, 0]));
    rejects("32-bit switch on INT32_MAX + 1", switchOn(T.i32, [2 ** 31]), /switch case out of range/);
    rejects("switch with a case twice", switchOn(T.i64, [5, 3, 5]), /duplicate switch case/);
    rejects("switch with INT64_MAX twice", switchOn(T.i64, [2n ** 63n - 1n, 0, 2n ** 63n - 1n]), /duplicate switch case/);
    rejects("switch on a double", switchOn(T.f64, [1]), /switch on a float/);
    rejects("switch to the block after the last", body({ ret: T.void, params: [T.i32] }, [[["Switch", 0, 0, 1, s(1), 1]]]), /block out of range/);
}

// Vectors and atomics.
{
    const vectorFunction = blocks => body({ ret: T.void, params: [T.v128] }, blocks);
    rejects("lane 6", vectorFunction([[["VAdd", b(6), 0, 0], ["RetVoid"]]]), /bad lane/);
    rejects("VRem of float lanes", vectorFunction([[["VRem", b(LANE.f32x4), b(1), 0, 0], ["RetVoid"]]]), /not defined for this lane shape/);
    rejects("VSqrt of integer lanes", vectorFunction([[["VSqrt", b(LANE.i32x4), 0], ["RetVoid"]]]), /not defined for this lane shape/);
    rejects("signedness 2", vectorFunction([[["VMin", b(LANE.i32x4), b(2), 0, 0], ["RetVoid"]]]), /bad signedness/);
    accepts("lane 3 of four", vectorFunction([[["VExtract", b(LANE.i32x4), b(0), b(3), 0], ["RetVoid"]]]));
    rejects("lane 4 of four", vectorFunction([[["VExtract", b(LANE.i32x4), b(0), b(4), 0], ["RetVoid"]]]), /lane index out of range/);
    rejects("lane 16 of sixteen replaced", vectorFunction([[["ConstI32", s(1)], ["VReplace", b(LANE.i8x16), b(16), 0, 1], ["RetVoid"]]]), /lane index out of range/);
    accepts("shuffle index 31", vectorFunction([[["VShuffle", 0, 0, { bytes: new Array(16).fill(31) }], ["RetVoid"]]]));
    rejects("shuffle index 32", vectorFunction([[["VShuffle", 0, 0, { bytes: [...new Array(15).fill(0), 32] }], ["RetVoid"]]]), /shuffle index out of range/);
    rejects("vector conversion 28", vectorFunction([[["VConvert", b(28), 0], ["RetVoid"]]]), /bad vector conversion/);
    if (isX86)
        accepts("the x86-64 vector conversions", vectorFunction([[["VConvert", b(26), 0], ["VConvert", b(27), 0], ["RetVoid"]]]));
    else
        rejects("an x86-64 vector conversion elsewhere", vectorFunction([[["VConvert", b(26), 0], ["RetVoid"]]]), /x86-64 vector conversion/);
    rejects("VNarrow of 8-bit lanes", vectorFunction([[["VNarrow", b(LANE.i8x16), b(1), 0, 0], ["RetVoid"]]]), /not defined for this lane shape/);
    rejects("VExtMul into 8-bit lanes", vectorFunction([[["VExtMul", b(LANE.i8x16), b(1), b(0), 0, 0], ["RetVoid"]]]), /bad VExtMul/);
    rejects("scalar Add of vectors", vectorFunction([[["Add", 0, 0], ["RetVoid"]]]), /scalar operation on a vector/);

    const pointerFunction = blocks => body({ ret: T.void, params: [T.i64] }, blocks);
    rejects("atomic load of a double", pointerFunction([[["AtomicLoad", b(MEM.f64), b(0), 0], ["RetVoid"]]]), /bad atomic kind/);
    rejects("atomic store of kind I8S", pointerFunction([[["ConstI32", s(1)], ["AtomicStore", b(MEM.i8s), b(0), 1, 0], ["RetVoid"]]]), /bad atomic kind/);
    accepts("atomic load of kind I8S", pointerFunction([[["AtomicLoad", b(MEM.i8s), b(0), 0], ["RetVoid"]]]));
    rejects("memory order 5", pointerFunction([[["AtomicLoad", b(MEM.i32), b(5), 0], ["RetVoid"]]]), /bad memory order/);
    rejects("fence of order 5", pointerFunction([[["Fence", b(5)], ["RetVoid"]]]), /bad memory order/);
    rejects("atomic operation 6", pointerFunction([[["ConstI32", s(1)], ["AtomicRmw", b(6), b(MEM.i32), b(4), 1, 0], ["RetVoid"]]]), /bad atomic operation/);
    rejects("compare-and-swap of an i64 with i32 values", pointerFunction([[["ConstI32", s(1)], ["AtomicCas", b(MEM.i64), b(4), b(4), 1, 1, 0], ["RetVoid"]]]), /operand has the wrong type/);
}

// Inline assembly and cpuid: x86-64 machine code.
{
    const nop = [0x90];
    const asm = ({ flags = 0, code = nop, inputs = [], outputs = [], clobbers = [] }) =>
        ["InlineAsm", b(flags), code.length, ...code.map(b), inputs.length, ...inputs.flatMap(([value, reg]) => [value, b(reg)]), outputs.length, ...outputs.flatMap(([type, reg]) => [b(type), b(reg)]), clobbers.length, ...clobbers.map(b)];
    const longAndDouble = blocks => body({ ret: T.void, params: [T.i64, T.f64] }, blocks);
    const RAX = 0, RCX = 1, RSP = 4, RBP = 5, XMM0 = 16;
    if (!isX86) {
        rejects("inline assembly in a module for another target", longAndDouble([[asm({}), ["RetVoid"]]]), /InlineAsm is x86-64 machine code/);
        rejects("cpuid in a module for another target", body({ ret: T.void, params: [T.i32] }, [[["CpuId", 0, 0], ["RetVoid"]]]), /CpuId is an x86-64 instruction/);
    } else {
        accepts("an input and an output in one register", longAndDouble([[asm({ inputs: [[0, RAX]], outputs: [[T.i64, RAX]] }), ["RetVoid"]]]));
        rejects("two outputs in one register", longAndDouble([[asm({ outputs: [[T.i64, RAX], [T.i64, RAX]] }), ["RetVoid"]]]), /two InlineAsm outputs in one register/);
        rejects("two inputs in one register", longAndDouble([[asm({ inputs: [[0, RCX], [0, RCX]] }), ["RetVoid"]]]), /two InlineAsm inputs in one register/);
        rejects("an output in a clobbered register", longAndDouble([[asm({ outputs: [[T.i64, RAX]], clobbers: [RAX] }), ["RetVoid"]]]), /operand is in a clobbered register/);
        rejects("an input in a clobbered register", longAndDouble([[asm({ inputs: [[0, RCX]], clobbers: [RCX] }), ["RetVoid"]]]), /operand is in a clobbered register/);
        accepts("a register clobbered twice", longAndDouble([[asm({ clobbers: [RCX, RCX] }), ["RetVoid"]]]));
        for (const [name, reg] of [["rsp", RSP], ["rbp", RBP], ["register 32", 32]]) {
            rejects(`an input in ${name}`, longAndDouble([[asm({ inputs: [[0, reg]] }), ["RetVoid"]]]), /bad InlineAsm input register/);
            rejects(`an output in ${name}`, longAndDouble([[asm({ outputs: [[T.i64, reg]] }), ["RetVoid"]]]), /bad InlineAsm output register/);
            rejects(`a clobber of ${name}`, longAndDouble([[asm({ clobbers: [reg] }), ["RetVoid"]]]), /bad InlineAsm clobber/);
        }
        rejects("an integer in a vector register", longAndDouble([[asm({ inputs: [[0, XMM0]] }), ["RetVoid"]]]), /bad InlineAsm input register/);
        rejects("a double in an integer register", longAndDouble([[asm({ inputs: [[1, RAX]] }), ["RetVoid"]]]), /bad InlineAsm input register/);
        rejects("a vector output in an integer register", longAndDouble([[asm({ outputs: [[T.v128, RAX]] }), ["RetVoid"]]]), /bad InlineAsm output register/);
        rejects("an output of type void", longAndDouble([[asm({ outputs: [[T.void, RAX]] }), ["RetVoid"]]]), /bad InlineAsm output type/);
        rejects("flags 2", longAndDouble([[asm({ flags: 2 }), ["RetVoid"]]]), /bad InlineAsm/);
        accepts("4096 bytes of code", longAndDouble([[asm({ code: new Array(4096).fill(0x90) }), ["RetVoid"]]]));
        rejects("4097 bytes of code", longAndDouble([[asm({ code: new Array(4097).fill(0x90) }), ["RetVoid"]]]), /bad InlineAsm/);
        const registers = [0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18];
        rejects("seventeen inputs", longAndDouble([[asm({ inputs: registers.map(reg => [reg < 16 ? 0 : 1, reg]) }), ["RetVoid"]]]), /bad InlineAsm input count/);
        rejects("seventeen outputs", longAndDouble([[asm({ outputs: registers.map(reg => [reg < 16 ? T.i64 : T.f64, reg]) }), ["RetVoid"]]]), /bad InlineAsm output count/);
        accepts("64 clobbers", longAndDouble([[asm({ clobbers: new Array(64).fill(RCX) }), ["RetVoid"]]]));
        rejects("65 clobbers", longAndDouble([[asm({ clobbers: new Array(65).fill(RCX) }), ["RetVoid"]]]), /bad InlineAsm clobber count/);
    }
}

// Exports, libraries, constructors and destructors.
{
    rejects("export of function 1 of 1", variant(m => { m.exports[0].func = 1; }), /export function out of range/);
    rejects("export of a function not flagged as exported", variant(m => { m.funcs[0].exported = false; }), /not flagged as exported/);
    accepts("a function flagged as exported and not exported to JavaScript", variant(m => { m.exports = []; }));
    rejects("export with one argument too few", variant(m => { m.exports[0].args = []; }), /export argument count does not match/);
    rejects("export with one argument too many", variant(m => { m.exports[0].args = [FFI.i32, FFI.i32]; }), /export argument count does not match/);
    rejects("export with return type 200", variant(m => { m.exports[0].ret = 200; }), /bad FFI type/);
    rejects("export with argument type 200", variant(m => { m.exports[0].args = [200]; }), /bad FFI type/);
    rejects("export with an argument of type void", variant(m => { m.exports[0].args = [FFI.void]; }), /signature JavaScript cannot call/);
    rejects("export of a variadic function", variant(m => { m.sigs[0] = { ...intToInt, variadic: true }; }), /does not have a scalar signature/);
    rejects("export of a function that returns two values", { sigs: [{ ret: [T.i64, T.f64], params: [] }], funcs: [{ name: "f", sig: 0, exported: true, blocks: [[["ConstI64", s(1)], ["ConstF64", { f64: 1 }], ["Ret", 0, 1]]] }], exports: [{ name: "f", func: 0, ret: FFI.i64, args: [] }] }, isWindows ? /more results/ : /does not have a scalar signature/);
    rejects("export of a function that takes a vector", { sigs: [{ ret: T.void, params: [T.v128] }], funcs: [{ name: "f", sig: 0, exported: true, blocks: [[["RetVoid"]]] }], exports: [{ name: "f", func: 0, ret: FFI.void, args: [FFI.ptr] }] }, isWindows ? /vector cannot be passed/ : /does not have a scalar signature/);
    const manyArguments = count => ({ sigs: [{ ret: T.i32, params: new Array(count).fill(T.i32) }], funcs: [{ name: "f", sig: 0, exported: true, blocks: [[["Ret", count - 1]]] }], exports: [{ name: "f", func: 0, ret: FFI.i32, args: new Array(count).fill(FFI.i32) }] });
    eq(accepts("export with 32 arguments", manyArguments(32)).f(...Array.from({ length: 32 }, (_, i) => i)), 31, "32 arguments arrive");
    rejects("export with 33 arguments", manyArguments(33), /signature JavaScript cannot call/);
    accepts("a function with 33 arguments that is not exported to JavaScript", { ...manyArguments(33), exports: [] });

    rejects("constructor 1 of 1", variant(m => { m.constructors = [1]; }), /constructor or destructor function out of range/);
    rejects("destructor 1 of 1", variant(m => { m.destructors = [1]; }), /constructor or destructor function out of range/);
    rejects("constructor that takes an argument", variant(m => { m.constructors = [0]; }), /must be void f\(void\)/);
    rejects("destructor that takes an argument", variant(m => { m.destructors = [0]; }), /must be void f\(void\)/);
    rejects("library nothing can load", variant(m => { m.libraries = ["no-such-library-anywhere"]; }), /^cannot load library 'no-such-library-anywhere': ./, "");
}

print("decoder rejects ok:", cases, "cases");
