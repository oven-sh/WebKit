//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// The bytes a segment starts as are not a count of things (at most 2^24 of those): a run of them is as long as its
// part of the segment has room for. Seventeen megabytes of each kind, the last byte the only one that is not zero.
load("./resources/bir-assembler.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n }), t8 = n => ({ u8: n });

const LENGTH = 17 * 1024 * 1024;
const PART = 32 * 1024 * 1024; // where the writable part starts: a multiple of 16384 past the constants
function run(last) { const bytes = new Uint8Array(LENGTH); bytes[LENGTH - 1] = last; return bytes; }

const intOfInt = { ret: T.i32, params: [T.i32] };
// int data8(int offset) { return data[offset]; }   int tls8(int offset) { return tls[offset]; }
const byteAt = (name, addressOp) => ({ name, sig: 0, exported: true, blocks: [[[addressOp, 0], ["SExt32", 0], ["Add", 1, 2], ["Load", t8(MEM.i8u), 3, s(0)], ["Ret", 4]]] });
const m = $vm.cModule(assemble({
    sigs: [intOfInt],
    data: { size: PART + LENGTH + 5, align: 16, readOnly: PART, constants: run(1), init: run(2), relocs: [] },
    tls: { size: LENGTH + 5, align: 16, init: run(3) },
    funcs: [byteAt("data8", "DataAddr"), byteAt("tls8", "TlsAddr")],
    exports: [{ name: "data8", func: 0, ret: FFI.i32, args: [FFI.i32] }, { name: "tls8", func: 1, ret: FFI.i32, args: [FFI.i32] }],
}));
eq(m.data8(LENGTH - 1), 1, "the last constant byte given");
eq(m.data8(LENGTH - 2), 0, "the constant byte before it");
eq(m.data8(LENGTH), 0, "the constant byte after it");
eq(m.data8(PART - 1), 0, "the last byte of the constant part");
eq(m.data8(PART + LENGTH - 1), 2, "the last writable byte given");
eq(m.data8(PART + LENGTH - 2), 0, "the writable byte before it");
eq(m.data8(PART + LENGTH), 0, "the writable byte after it");
eq(m.data8(PART + LENGTH + 4), 0, "the last byte of the data");
eq(m.tls8(LENGTH - 1), 3, "the last thread-local byte given");
eq(m.tls8(LENGTH - 2), 0, "the thread-local byte before it");
eq(m.tls8(LENGTH + 4), 0, "the last thread-local byte");
print("large data ok");
