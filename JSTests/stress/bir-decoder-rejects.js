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
// The decoder goes by the target the module says it is for, and only then does the loader compare that with the
// machine: a module for another target that decodes is refused by the loader, one that does not by the decoder.
const X86_64 = 0, ARM64 = 1, LINUX = 0, DARWIN = 1, WINDOWS = 2;
const targets = [[X86_64, LINUX], [ARM64, LINUX], [X86_64, DARWIN], [ARM64, DARWIN], [X86_64, WINDOWS]];
function forTarget(targetArch, targetOS, module) { const bytes = new Uint8Array(bytesOf(module)); bytes[4] = targetArch; bytes[5] = targetOS; return bytes; }
function decodes(what, targetArch, targetOS, module) {
    if (targetArch === arch && targetOS === os)
        return void accepts(what, forTarget(targetArch, targetOS, module));
    rejects(what, forTarget(targetArch, targetOS, module), /^BIR module was compiled for a different target$/, "");
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
    rejects("os 3", patched(5, 3), /unsupported target/);
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
        // What a call passes in: 16 bytes for each argument, and each by-value one's size and alignment, under 512 MiB.
        accepts("511 by-value arguments of a megabyte", taking(new Array(511).fill({ byval: 1 << 20 })));
        rejects("512 by-value arguments of a megabyte", taking(new Array(512).fill({ byval: 1 << 20 })), /arguments are too large/);
        rejects("1025 by-value arguments of a megabyte", taking(new Array(1025).fill({ byval: 1 << 20 })), /arguments are too large/);
        accepts("511 by-value arguments of a megabyte and 60000 integers", taking([...new Array(511).fill({ byval: 1 << 20 }), ...new Array(60000).fill(T.i64)]));
        rejects("511 by-value arguments of a megabyte and 70000 integers", taking([...new Array(511).fill({ byval: 1 << 20 }), ...new Array(70000).fill(T.i64)]), /arguments are too large/);
        // The anonymous arguments of a variadic call count like the named ones.
        {
            const variadicCall = anonymous => {
                const block = new Block(0);
                const address = block.def("SlotAddr", 0);
                const word = block.def("ConstI64", s(1));
                block.run("Call", 0, 511 + anonymous, ...new Array(511).fill(address), ...new Array(anonymous).fill(word));
                block.run("RetVoid");
                return {
                    sigs: [{ ret: T.void, params: new Array(511).fill({ byval: 1 << 20 }), variadic: true }, { ret: T.void, params: [] }],
                    funcs: [{ name: "callee", sig: 0, noinline: true, blocks: [[["RetVoid"]]] }, { name: "f", sig: 1, exported: true, slots: [{ size: 1 << 20, align: 8 }], blocks: [block.insts] }],
                };
            };
            accepts("a variadic call with 511 by-value megabytes and 60000 anonymous arguments", variadicCall(60000));
            rejects("a variadic call with 511 by-value megabytes and 70000 anonymous arguments", variadicCall(70000), /arguments are too large/);
        }
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
    const PART = 16384;
    rejects("a constant part longer than the data", withData({ size: 8, readOnly: 16 }), /bad data segment size/);
    rejects("data over 4 GiB", withData({ size: 2 ** 32 + 1 }), /bad data segment size/);
    // The two runs of bytes: each as long as its part at most, and all there.
    accepts("as many initialized bytes as data", withData({ size: 3, init: [1, 2, 3] }));
    rejects("one more initialized byte than data", withData({ size: 2, init: [1, 2, 3] }), /more initialized data than the writable part/);
    accepts("as many constant bytes as the constant part", withData({ size: 3, readOnly: 3, constants: [1, 2, 3] }));
    rejects("one more constant byte than the constant part", withData({ size: 3, readOnly: 2, constants: [1, 2, 3] }), /writable part of the data segment does not start at a multiple of 16384/);
    rejects("one more constant byte than the constant part, which is all there is", withData({ size: 2, readOnly: 2, constants: [1, 2, 3] }), /more constant data than the constant part/);
    rejects("one more constant byte than a constant part of 16384", withData({ size: PART + 8, readOnly: PART, constants: new Array(PART + 1).fill(1) }), /more constant data than the constant part/);
    accepts("both parts filled", withData({ size: PART + 8, readOnly: PART, constants: new Array(PART).fill(1), init: new Array(8).fill(2) }));
    rejects("one more initialized byte than the writable part", withData({ size: PART + 8, readOnly: PART, init: new Array(9).fill(2) }), /more initialized data than the writable part/);
    rejects("an initialized byte and no writable part", withData({ size: PART, readOnly: PART, init: [1] }), /more initialized data than the writable part/);
    // A length is checked against what the segment has room for, then against what is left of the input.
    {
        // `run` is "constants" or "init": that run said to be as long as everything after its length, and one byte longer.
        const marker = [0xab, 0xcd, 0xef];
        const restOfInput = (run, data) => {
            const bytes = assemble(withData({ ...data, [run]: marker }));
            const start = bytes.findIndex((_, i) => marker.every((byte, j) => bytes[i + j] === byte));
            return bytes.length - start;
        };
        for (const [run, lengthField, data] of [["constants", "constantBytes", { size: PART, readOnly: PART }], ["init", "initBytes", { size: PART }]]) {
            const rest = restOfInput(run, data);
            rejects(`${run}: a run as long as the rest of the input`, withData({ ...data, [run]: marker, [lengthField]: rest }), /bad varuint|unexpected end of input/);
            rejects(`${run}: a run one byte longer than the rest of the input`, withData({ ...data, [run]: marker, [lengthField]: rest + 1 }), /unexpected end of input/);
            accepts(`${run}: a run of the three bytes that are there`, withData({ ...data, [run]: marker, [lengthField]: 3 }));
        }
    }
    rejects("an initialized run of 4 GiB in a few bytes", withData({ size: 2 ** 32, initBytes: 2 ** 32 }), /unexpected end of input/);
    rejects("an initialized run of 2^63 bytes", withData({ size: 2 ** 32, initBytes: 2n ** 63n }), /more initialized data than the writable part/);
    rejects("a constant run of 2^64 - 1 bytes", withData({ size: PART, readOnly: PART, constantBytes: 2n ** 64n - 1n }), /more constant data than the constant part/);
    // The length of a run is not a count of things: it may be more than 2^24 (bir-large-data.js loads one that is).
    rejects("an initialized run of 2^24 + 1 bytes that is not there", withData({ size: 2 ** 25, initBytes: 2 ** 24 + 1 }), /unexpected end of input/);
    rejects("a constant run of 2^24 + 1 bytes that is not there", withData({ size: 2 ** 25, readOnly: 2 ** 25, constantBytes: 2 ** 24 + 1 }), /unexpected end of input/);
    // Where the writable part starts.
    for (const readOnly of [1, 100, 4096, 8192, PART - 1, PART + 1, PART + 4096])
        rejects(`a writable part at ${readOnly}`, withData({ size: 2 * PART, readOnly }), /writable part of the data segment does not start at a multiple of 16384/);
    for (const readOnly of [0, PART, 2 * PART])
        accepts(`a writable part at ${readOnly}`, withData({ size: 2 * PART + 1, readOnly }));
    for (const size of [1, 100, 4096, PART - 1, PART + 1])
        accepts(`a constant part of ${size} bytes and no writable part`, withData({ size, readOnly: size }));
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
    accepts("thread-local data of 256 MiB", withTls({ size: 2 ** 28 }));
    accepts("as many initialized thread-local bytes as there is thread-local data", withTls({ size: 2, init: [1, 2] }));
    rejects("one more initialized thread-local byte than there is thread-local data", withTls({ size: 1, init: [1, 2] }), /more initialized thread-local data than the thread-local segment/);
    rejects("an initialized byte and no thread-local data", withTls({ size: 0, init: [1] }), /more initialized thread-local data than the thread-local segment/);
    {
        const marker = [0xab, 0xcd, 0xef];
        const bytes = assemble(withTls({ size: 64, init: marker }));
        const rest = bytes.length - bytes.findIndex((_, i) => marker.every((byte, j) => bytes[i + j] === byte));
        rejects("a thread-local image as long as the rest of the input", withTls({ size: 64, init: marker, initBytes: rest }), /bad varuint|unexpected end of input/);
        rejects("a thread-local image one byte longer than the rest of the input", withTls({ size: 64, init: marker, initBytes: rest + 1 }), /unexpected end of input/);
    }
    rejects("a thread-local image of 2^24 + 1 bytes that is not there", withTls({ size: 2 ** 25, initBytes: 2 ** 24 + 1 }), /unexpected end of input/);
    rejects("a thread-local image of 2^63 bytes", withTls({ size: 2 ** 28, initBytes: 2n ** 63n }), /more initialized thread-local data than the thread-local segment/);
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
    // The slots of a function, each rounded up to 16 and with its alignment: 512 MiB.
    accepts("slots of 256 MiB and of 256 MiB less 32 bytes", voidFunction([[["RetVoid"]]], { slots: [{ size: 2 ** 28, align: 16 }, { size: 2 ** 28 - 32, align: 16 }] }));
    rejects("slots of 256 MiB and of 256 MiB less 16 bytes", voidFunction([[["RetVoid"]]], { slots: [{ size: 2 ** 28, align: 16 }, { size: 2 ** 28 - 16, align: 16 }] }), /stack frame is too large/);
    rejects("two slots of 256 MiB", voidFunction([[["RetVoid"]]], { slots: new Array(2).fill({ size: 2 ** 28, align: 16 }) }), /stack frame is too large/);
    rejects("three slots of 256 MiB", voidFunction([[["RetVoid"]]], { slots: new Array(3).fill({ size: 2 ** 28, align: 16 }) }), /stack frame is too large/);
    rejects("seventeen slots of 256 MiB", voidFunction([[["RetVoid"]]], { slots: new Array(17).fill({ size: 2 ** 28, align: 16 }) }), /stack frame is too large/);
    accepts("five hundred slots of a megabyte", voidFunction([[["RetVoid"]]], { slots: new Array(500).fill({ size: 1 << 20, align: 16 }) }));
    rejects("512 slots of a megabyte", voidFunction([[["RetVoid"]]], { slots: new Array(512).fill({ size: 1 << 20, align: 16 }) }), /stack frame is too large/);
    // A frame is the slots and the largest argument area: both as large as they get, it is compiled (and not run: no
    // thread has a stack for it), as it is when a callee that asks to be inlined would bring its own copies of what
    // it is passed by value into a frame with no room for them.
    if (!isWindows) {
        for (const alwaysInline of [false, true]) {
            const block = new Block(0);
            const address = block.def("SlotAddr", 0);
            block.run("Call", 0, 511, ...new Array(511).fill(address));
            block.run("Call", 0, 511, ...new Array(511).fill(address));
            block.run("RetVoid");
            accepts(`slots of 511 MiB and two calls that pass 511 MiB by value${alwaysInline ? " to an always_inline callee" : ""}`, {
                sigs: [{ ret: T.void, params: new Array(511).fill({ byval: 1 << 20 }) }, { ret: T.void, params: [] }],
                funcs: [
                    { name: "callee", sig: 0, alwaysInline, noinline: !alwaysInline, blocks: [[["Load", b(MEM.i8u), 510, s(0)], ["RetVoid"]]] },
                    { name: "f", sig: 1, exported: true, slots: [{ size: 2 ** 28, align: 8 }, { size: 2 ** 28 - (1 << 20), align: 8 }], blocks: [block.insts] },
                ],
            });
        }
    }

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
    rejects("FMin of integers", intFunction([[["FMin", 0, 0], ["Ret", 1]]]), /FMin and FMax operands must be the same floating-point type/);
    rejects("FMax of a float and a double", voidFunction([[["ConstF64", { f64: 1 }], ["FDemote", 0], ["FMax", 0, 1], ["RetVoid"]]]), /FMin and FMax operands must be the same floating-point type/);
    accepts("FMin of doubles and FMax of floats", voidFunction([[["ConstF64", { f64: 1 }], ["FDemote", 0], ["FMin", 0, 0], ["FMax", 1, 1], ["RetVoid"]]]));
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
    rejects("VFMin of integer lanes", vectorFunction([[["VFMin", b(LANE.i32x4), 0, 0], ["RetVoid"]]]), /not defined for this lane shape/);
    rejects("VFMax of lane 6", vectorFunction([[["VFMax", b(6), 0, 0], ["RetVoid"]]]), /bad lane/);
    accepts("VFMin and VFMax of both float lane shapes", vectorFunction([[["VFMin", b(LANE.f32x4), 0, 0], ["VFMax", b(LANE.f64x2), 0, 1], ["RetVoid"]]]));
    rejects("VSqrt of integer lanes", vectorFunction([[["VSqrt", b(LANE.i32x4), 0], ["RetVoid"]]]), /not defined for this lane shape/);
    rejects("signedness 2", vectorFunction([[["VMin", b(LANE.i32x4), b(2), 0, 0], ["RetVoid"]]]), /bad signedness/);
    accepts("lane 3 of four", vectorFunction([[["VExtract", b(LANE.i32x4), b(0), b(3), 0], ["RetVoid"]]]));
    rejects("lane 4 of four", vectorFunction([[["VExtract", b(LANE.i32x4), b(0), b(4), 0], ["RetVoid"]]]), /lane index out of range/);
    rejects("lane 16 of sixteen replaced", vectorFunction([[["ConstI32", s(1)], ["VReplace", b(LANE.i8x16), b(16), 0, 1], ["RetVoid"]]]), /lane index out of range/);
    accepts("shuffle index 31", vectorFunction([[["VShuffle", 0, 0, { bytes: new Array(16).fill(31) }], ["RetVoid"]]]));
    rejects("shuffle index 32", vectorFunction([[["VShuffle", 0, 0, { bytes: [...new Array(15).fill(0), 32] }], ["RetVoid"]]]), /shuffle index out of range/);
    accepts("vector conversion 25", vectorFunction([[["VConvert", b(25), 0], ["RetVoid"]]]));
    rejects("vector conversion 26", vectorFunction([[["VConvert", b(26), 0], ["RetVoid"]]]), /bad vector conversion/);
    rejects("vector conversion 255", vectorFunction([[["VConvert", b(255), 0], ["RetVoid"]]]), /bad vector conversion/);
    rejects("vector conversion of a scalar", body({ ret: T.void, params: [T.f64] }, [[["VConvert", b(0), 0], ["RetVoid"]]]), /operand has the wrong type/);
    rejects("VNarrow of 8-bit lanes", vectorFunction([[["VNarrow", b(LANE.i8x16), b(1), 0, 0], ["RetVoid"]]]), /not defined for this lane shape/);
    rejects("VExtMul into 8-bit lanes", vectorFunction([[["VExtMul", b(LANE.i8x16), b(1), b(0), 0, 0], ["RetVoid"]]]), /bad VExtMul/);
    rejects("scalar Add of vectors", vectorFunction([[["Add", 0, 0], ["RetVoid"]]]), /scalar operation on a vector/);

    const pointerFunction = blocks => body({ ret: T.void, params: [T.i64] }, blocks);
    rejects("atomic load of a double", pointerFunction([[["AtomicLoad", b(MEM.f64), b(0), 0], ["RetVoid"]]]), /bad atomic kind/);
    rejects("atomic store of kind I8S", pointerFunction([[["ConstI32", s(1)], ["AtomicStore", b(MEM.i8s), b(0), 1, 0], ["RetVoid"]]]), /bad atomic kind/);
    accepts("atomic load of kind I8S", pointerFunction([[["AtomicLoad", b(MEM.i8s), b(0), 0], ["RetVoid"]]]));
    rejects("memory order 5", pointerFunction([[["AtomicLoad", b(MEM.i32), b(5), 0], ["RetVoid"]]]), /bad memory order/);
    rejects("fence of order 5", pointerFunction([[["Fence", b(5)], ["RetVoid"]]]), /bad memory order/);
    for (let order = 0; order < 5; order++)
        accepts(`a fence of order ${order} for the compiler only`, pointerFunction([[["Fence", b(0x80 | order)], ["RetVoid"]]]));
    rejects("a fence of order 5 for the compiler only", pointerFunction([[["Fence", b(0x85)], ["RetVoid"]]]), /bad memory order/);
    rejects("a fence of order 0x41", pointerFunction([[["Fence", b(0x41)], ["RetVoid"]]]), /bad memory order/);
    rejects("an atomic load of order 0x81", pointerFunction([[["AtomicLoad", b(MEM.i32), b(0x81), 0], ["RetVoid"]]]), /bad memory order/);
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
        for (let flags = 0; flags < 8; flags++)
            accepts(`flags ${flags}`, longAndDouble([[asm({ flags }), ["RetVoid"]]]));
        rejects("flags 8", longAndDouble([[asm({ flags: 8 }), ["RetVoid"]]]), /bad InlineAsm/);
        rejects("flags 0x81", longAndDouble([[asm({ flags: 0x81 }), ["RetVoid"]]]), /bad InlineAsm/);
        accepts("4096 bytes of code", longAndDouble([[asm({ code: new Array(4096).fill(0x90) }), ["RetVoid"]]]));
        rejects("4097 bytes of code", longAndDouble([[asm({ code: new Array(4097).fill(0x90) }), ["RetVoid"]]]), /bad InlineAsm/);
        const registers = [0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18];
        rejects("seventeen inputs", longAndDouble([[asm({ inputs: registers.map(reg => [reg < 16 ? 0 : 1, reg]) }), ["RetVoid"]]]), /bad InlineAsm input count/);
        rejects("seventeen outputs", longAndDouble([[asm({ outputs: registers.map(reg => [reg < 16 ? T.i64 : T.f64, reg]) }), ["RetVoid"]]]), /bad InlineAsm output count/);
        accepts("64 clobbers", longAndDouble([[asm({ clobbers: new Array(64).fill(RCX) }), ["RetVoid"]]]));
        rejects("65 clobbers", longAndDouble([[asm({ clobbers: new Array(65).fill(RCX) }), ["RetVoid"]]]), /bad InlineAsm clobber count/);
    }
}

// What each target has and has not, whichever machine this is.
{
    const returning = (ret, values) => body({ ret, params: [] }, [[...values, ["Ret", ...values.map((_, i) => i)]]]);
    const taking = params => body({ ret: T.void, params }, [[["RetVoid"]]]);
    const i64 = ["ConstI64", s(1)], f64 = ["ConstF64", { f64: 1 }], f32 = ["ConstF32", { bytes: [0, 0, 128, 63] }];
    const nop = ["InlineAsm", b(0), 1, b(0x90), 0, 0, 0];
    // void callee(void*, ...); void f(void* p) { callee(p, <`before` integers>, *(vector*)p); }
    const anonymousVector = before => {
        const block = new Block(1);
        const vector = block.def("Load", b(MEM.v128), 0, s(0));
        const word = block.def("ConstI64", s(1));
        block.run("Call", 0, 2 + before, 0, ...new Array(before).fill(word), vector);
        block.run("RetVoid");
        return {
            sigs: [{ ret: T.void, params: [T.i64], variadic: true }, { ret: T.void, params: [T.i64] }],
            funcs: [{ name: "callee", sig: 0, noinline: true, blocks: [[["RetVoid"]]] }, { name: "f", sig: 1, exported: true, blocks: [block.insts] }],
        };
    };
    const anonymousScalars = () => {
        const block = new Block(1);
        block.run("Call", 0, 3, 0, block.def("ConstF64", { f64: 1 }), block.def("ConstI64", s(1)));
        block.run("RetVoid");
        return {
            sigs: [{ ret: T.void, params: [T.i64], variadic: true }, { ret: T.void, params: [T.i64] }],
            funcs: [{ name: "callee", sig: 0, noinline: true, blocks: [[["RetVoid"]]] }, { name: "f", sig: 1, exported: true, blocks: [block.insts] }],
        };
    };
    for (const [targetArch, targetOS] of targets) {
        const name = `${["x86-64", "arm64"][targetArch]} ${["linux", "darwin", "windows"][targetOS]}`;
        const refused = (what, module, pattern) => rejects(`${name}: ${what}`, forTarget(targetArch, targetOS, module), pattern);
        const decoded = (what, module) => decodes(`${name}: ${what}`, targetArch, targetOS, module);
        decoded("nothing special", taking([T.i32, T.f64, T.i64]));
        decoded("anonymous integer and floating-point arguments", anonymousScalars());
        if (targetOS === WINDOWS) {
            refused("a vector by value", taking([T.v128]), /a vector cannot be passed by value on this target/);
            refused("an aggregate in the stack arguments", taking([{ byval: 24 }]), /an aggregate cannot be passed in the stack arguments on this target/);
            refused("two results", returning([T.i64, T.i64], [i64, i64]), /more results than the target has result registers/);
            refused("an integer and a floating-point result", returning([T.i64, T.f64], [i64, f64]), /more results than the target has result registers/);
            decoded("one result", returning([T.f64], [f64]));
        } else {
            decoded("a vector by value", taking([T.v128]));
            decoded("an aggregate in the stack arguments", taking([{ byval: 24 }]));
            decoded("two integer and two floating-point results", returning([T.i64, T.i64, T.f64, T.f64], [i64, i64, f64, f64]));
        }
        if (targetOS === WINDOWS || (targetOS === DARWIN && targetArch === ARM64)) {
            for (const before of [0, 1, 4, 9])
                refused(`an anonymous vector argument after ${before} others`, anonymousVector(before), /a vector cannot be one of the anonymous arguments of a variadic call on this target/);
        } else
            decoded("an anonymous vector argument", anonymousVector(2));
        if (targetArch === ARM64) {
            if (targetOS !== WINDOWS)
                decoded("four float results", returning([T.f32, T.f32, T.f32, T.f32], [f32, f32, f32, f32]));
            refused("inline assembly", body({ ret: T.void, params: [] }, [[nop, ["RetVoid"]]]), /InlineAsm is x86-64 machine code/);
            refused("cpuid", body({ ret: T.void, params: [T.i32] }, [[["CpuId", 0, 0], ["RetVoid"]]]), /CpuId is an x86-64 instruction/);
        } else {
            if (targetOS !== WINDOWS)
                refused("three double results", returning([T.f64, T.f64, T.f64], [f64, f64, f64]), /more results than the target has result registers/);
            decoded("inline assembly", body({ ret: T.void, params: [] }, [[nop, ["RetVoid"]]]));
            // xmm0..15 are registers 16..31. Win64 callers expect xmm6 and up kept, so there they are not the statement's.
            const withVectorRegister = (reg, how) => body({ ret: T.void, params: [T.f64] }, [[
                ["InlineAsm", b(0), 1, b(0x90), ...(how === "input" ? [1, 0, b(reg)] : [0]), ...(how === "output" ? [1, b(T.f64), b(reg)] : [0]), ...(how === "clobber" ? [1, b(reg)] : [0])],
                ["RetVoid"]]]);
            for (const how of ["input", "output", "clobber"]) {
                decoded(`xmm5 as an ${how} of inline assembly`, withVectorRegister(21, how));
                for (const reg of [22, 27, 31]) {
                    if (targetOS === WINDOWS)
                        refused(`xmm${reg - 16} as an ${how} of inline assembly`, withVectorRegister(reg, how), /bad InlineAsm (input register|output register|clobber)/);
                    else
                        decoded(`xmm${reg - 16} as an ${how} of inline assembly`, withVectorRegister(reg, how));
                }
            }
            decoded("cpuid", body({ ret: T.void, params: [T.i32] }, [[["CpuId", 0, 0], ["RetVoid"]]]));
        }
    }
    rejects("arm64 windows", forTarget(ARM64, WINDOWS, taking([T.i32])), /./, "");
}

// Operands of the wrong kind, for the operations whose rule is their own.
{
    // value ids: 0 f64, 1 i32, 2 i64, 3 v128, 4 f32
    const mixed = blocks => body({ ret: T.void, params: [T.f64, T.i32, T.i64, T.v128, T.f32] }, blocks);
    const refused = (what, inst, pattern) => rejects(what, mixed([[inst, ["RetVoid"]]]), pattern);
    const accepted = (what, inst) => accepts(what, mixed([[inst, ["RetVoid"]]]));
    for (const op of ["RotL", "RotR"]) {
        refused(`${op} of a double`, [op, 0, 1], /rotate of a non-integer/);
        refused(`${op} of a vector`, [op, 3, 1], /rotate of a non-integer/);
        refused(`${op} by an i64`, [op, 2, 2], /operand has the wrong type/);
        accepted(`${op} of an i64 by an i32`, [op, 2, 1]);
    }
    for (const op of ["MulHigh", "UMulHigh"]) {
        refused(`${op} of doubles`, [op, 0, 0], /MulHigh operands must be the same integer type/);
        refused(`${op} of an i32 and an i64`, [op, 1, 2], /MulHigh operands must be the same integer type/);
        refused(`${op} of vectors`, [op, 3, 3], /MulHigh operands must be the same integer type/);
        accepted(`${op} of two i32`, [op, 1, 1]);
    }
    for (const op of ["Clz", "Ctz", "Popcnt", "Bswap"]) {
        refused(`${op} of a double`, [op, 0], /bit operation on a float/);
        refused(`${op} of a float`, [op, 4], /bit operation on a float/);
        refused(`${op} of a vector`, [op, 3], /bit operation on a float/);
        accepted(`${op} of an i64`, [op, 2]);
    }
    for (const op of ["Eq", "Ne", "Lt", "Le", "Gt", "Ge", "ULt", "ULe", "UGt", "UGe"]) {
        refused(`${op} of an i32 and an i64`, [op, 1, 2], /compare operands differ in type/);
        refused(`${op} of a float and a double`, [op, 4, 0], /compare operands differ in type/);
        refused(`${op} of vectors`, [op, 3, 3], /scalar operation on a vector/);
    }
    refused("Neg of a vector", ["Neg", 3], /scalar operation on a vector/);
    for (const op of ["Sub", "Mul", "Div", "And", "Xor"])
        refused(`${op} of vectors`, [op, 3, 3], /scalar operation on a vector/);
    for (const op of ["SToF", "UToF"]) {
        refused(`${op} to an i32`, [op, b(T.i32), 1], /bad int-to-float conversion/);
        refused(`${op} to a vector`, [op, b(T.v128), 1], /bad int-to-float conversion/);
        refused(`${op} of a double`, [op, b(T.f64), 0], /bad int-to-float conversion/);
        refused(`${op} of a vector`, [op, b(T.f32), 3], /bad int-to-float conversion/);
        accepted(`${op} of an i64 to a float`, [op, b(T.f32), 2]);
    }
    for (const op of ["FToS", "FToU"]) {
        refused(`${op} to a double`, [op, b(T.f64), 0], /bad float-to-int conversion/);
        refused(`${op} to a vector`, [op, b(T.v128), 0], /bad float-to-int conversion/);
        refused(`${op} of an i32`, [op, b(T.i32), 1], /bad float-to-int conversion/);
        refused(`${op} of a vector`, [op, b(T.i32), 3], /bad float-to-int conversion/);
        accepted(`${op} of a float to an i64`, [op, b(T.i64), 4]);
    }
    rejects("LocalSet of a local there is none of", mixed([[["LocalSet", 0, 1], ["RetVoid"]]]), /local out of range/);
    rejects("LocalSet of local 1 of 1", body({ ret: T.void, params: [T.i32] }, [[["LocalSet", 1, 0], ["RetVoid"]]], { locals: [T.i32] }), /local out of range/);
    rejects("LocalGet of local 1 of 1", body({ ret: T.void, params: [T.i32] }, [[["LocalGet", 1], ["RetVoid"]]], { locals: [T.i32] }), /local out of range/);
    rejects("LocalSet of an i64 into an i32", body({ ret: T.void, params: [T.i64] }, [[["LocalSet", 0, 0], ["RetVoid"]]], { locals: [T.i32] }), /wrong type/);
    refused("VExtMul of half 2", ["VExtMul", b(LANE.i16x8), b(0), b(2), 3, 3], /bad VExtMul/);
    refused("VExtMul into float lanes", ["VExtMul", b(LANE.f32x4), b(0), b(0), 3, 3], /bad VExtMul|not defined for this lane shape/);
    refused("VAvgU of 32-bit lanes", ["VAvgU", b(LANE.i32x4), 3, 3], /not defined for this lane shape/);
    refused("VAddSat of 32-bit lanes", ["VAddSat", b(LANE.i32x4), b(0), 3, 3], /not defined for this lane shape/);
    refused("VSubSat of 64-bit lanes", ["VSubSat", b(LANE.i64x2), b(0), 3, 3], /not defined for this lane shape/);
    refused("VShl of float lanes", ["VShl", b(LANE.f32x4), 3, 1], /not defined for this lane shape/);
    refused("VShrU of double lanes", ["VShrU", b(LANE.f64x2), 3, 1], /not defined for this lane shape/);
    refused("VBitmask of double lanes", ["VBitmask", b(LANE.f64x2), 3], /not defined for this lane shape/);
    refused("VAllTrue of float lanes", ["VAllTrue", b(LANE.f32x4), 3], /not defined for this lane shape/);
    refused("VNarrow of 64-bit lanes", ["VNarrow", b(LANE.i64x2), b(0), 3, 3], /not defined for this lane shape/);
}

// Numbers as they are written: a count is at most 2^24, an index fits in 32 bits, a varuint in 64.
{
    const header = write => { const w = new W(); w.raw([0x42, 0x49, 0x52, 0x30]).u8(arch).u8(os).u8(8).u8(0); write(w); return w.bytes(); };
    rejects("2^24 + 1 signatures", header(w => w.uv(2 ** 24 + 1)), /count too large/);
    rejects("2^32 - 2 signatures", header(w => w.uv(2 ** 32 - 2)), /count too large/);
    rejects("2^32 - 1 signatures", header(w => w.uv(2 ** 32 - 1)), /index too large/);
    rejects("2^32 signatures", header(w => w.uv(2 ** 32)), /index too large/);
    rejects("2^64 - 1 signatures", header(w => w.uv(2n ** 64n - 1n)), /index too large/);
    rejects("2^24 signatures that are not there", header(w => w.uv(2 ** 24)), /bad varuint|unexpected end of input/);
    rejects("2^24 + 1 results", header(w => w.uv(1).uv(2 ** 24 + 1)), /count too large/);
    rejects("2^24 + 1 parameters", header(w => w.uv(1).uv(0).u8(0).uv(2 ** 24 + 1)), /count too large/);
    rejects("2^24 + 1 externs", header(w => w.uv(0).uv(2 ** 24 + 1)), /count too large/);
    rejects("an extern with a name of 2^24 + 1 bytes", header(w => w.uv(0).uv(1).uv(2 ** 24 + 1)), /count too large/);
    rejects("2^24 + 1 relocations", header(w => w.uv(0).uv(0).uv(8).uv(8).uv(0).uv(0).uv(0).uv(2 ** 24 + 1)), /count too large/);
    rejects("a count written in eleven bytes", header(w => w.raw([0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00])), /bad varuint/);
    rejects("a count of ten bytes with bits past the sixty-fourth", header(w => w.raw([0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02])), /bad varuint/);
    rejects("no signatures, written in two bytes, and nothing after", header(w => w.raw([0x80, 0x00])), /bad varuint|unexpected end of input/);
    rejects("an extern whose signature is 2^32 - 1", header(w => w.uv(0).uv(1).str("x").u8(0).uv(2 ** 32 - 1)), /index too large/);
    rejects("an extern whose signature is 2^32", header(w => w.uv(0).uv(1).str("x").u8(0).uv(2 ** 32)), /index too large/);
    rejects("an extern whose signature is 2^32 - 2", header(w => w.uv(0).uv(1).str("x").u8(0).uv(2 ** 32 - 2)), /extern signature out of range/);
    rejects("value 2^32 - 1", voidFunction([[["Neg", 2 ** 32 - 1], ["RetVoid"]]]), /index too large/);
    rejects("value 2^32 - 2", voidFunction([[["Neg", 2 ** 32 - 2], ["RetVoid"]]]), /use of an undefined value/);
    rejects("a call with 2^24 + 1 arguments", { sigs: [{ ret: T.void, params: [], variadic: true }], funcs: [{ name: "f", sig: 0, exported: true, blocks: [[["Call", 0, 2 ** 24 + 1], ["RetVoid"]]] }] }, /count too large/);
    rejects("a switch with 2^24 + 1 cases", body({ ret: T.void, params: [T.i32] }, [[["Switch", 0, 0, 2 ** 24 + 1]]]), /count too large/);
    rejects("a block of 2^24 + 1 instructions", voidFunction([{ length: 2 ** 24 + 1, [Symbol.iterator]: function* () { yield ["RetVoid"]; } }]), /count too large/);
    rejects("a constant written in eleven bytes", voidFunction([[["ConstI64", { bytes: [0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00] }], ["RetVoid"]]]), /bad varint/);
    rejects("a constant of ten bytes with bits past the sixty-fourth", voidFunction([[["ConstI64", { bytes: [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02] }], ["RetVoid"]]]), /bad varint/);
    accepts("the least i64", voidFunction([[["ConstI64", s(-(2n ** 63n))], ["RetVoid"]]]));
    accepts("the greatest i64", voidFunction([[["ConstI64", s(2n ** 63n - 1n)], ["RetVoid"]]]));
    accepts("a constant written with a byte of padding", voidFunction([[["ConstI64", { bytes: [0x81, 0x00] }], ["RetVoid"]]]));
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
