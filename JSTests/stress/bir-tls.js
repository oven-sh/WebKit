//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

load("./resources/bir-assembler.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
// A library every system of that kind has without any development package, and a function in it that the jsc
// binary does not already have: glibc's libresolv.so.2 (a name with a dot in it is used as it is); elsewhere zlib
// under its bare name, which means what -lz does.
const [arch, os] = $vm.cModuleHost();
const onLinux = os === 0;
const library = onLinux ? "libresolv.so.2" : "z";
const module = assemble({
    sigs: [{ ret: T.i32, params: [] }, { ret: T.i64, params: [T.i64] }, { ret: T.i64, params: [T.i64, T.i64, T.i32] }, { ret: T.i64, params: [T.i64, T.i32] }],
    externs: [{ name: "ns_get32", sig: 1, kind: onLinux ? 0 : 0x80 }, { name: "crc32", sig: 2, kind: onLinux ? 0x80 : 0 }],
    // _Thread_local int counter = 40; _Thread_local long other;   (other at offset 8, zero)
    tls: { size: 16, align: 8, init: [40, 0, 0, 0] },
    libraries: [library],
    funcs: [
        // int bump(void) { other += 2; return ++counter; }
        { name: "bump", sig: 0, exported: true, blocks: [[
            ["TlsAddr", 8], ["Load", t8(MEM.i64), 0, s(0)], ["ConstI64", s(2)], ["Add", 1, 2], ["Store", t8(MEM.i64), 3, 0, s(0)],
            ["TlsAddr", 0], ["Load", t8(MEM.i32), 4, s(0)], ["ConstI32", s(1)], ["Add", 5, 6], ["Store", t8(MEM.i32), 7, 4, s(0)], ["Ret", 7],
        ]] },
        // unsigned long bigEndian(const unsigned char* p) { return ns_get32(p); }
        { name: "bigEndian", sig: 1, exported: true, blocks: [[["CallExtern", 0, 1, 0], ["Ret", 1]]] },
        // unsigned long crc(const void* p, unsigned n) { return crc32(0, p, n); }
        { name: "crc", sig: 3, exported: true, blocks: [[["ConstI64", s(0)], ["CallExtern", 1, 3, 2, 0, 1], ["Ret", 3]]] },
    ],
    exports: [
        { name: "bump", func: 0, ret: FFI.i32, args: [] },
        { name: "bigEndian", func: 1, ret: FFI.u64, args: [FFI.ptr] },
        { name: "crc", func: 2, ret: FFI.u64, args: [FFI.ptr, FFI.u32] },
    ],
});
const m = $vm.cModule(module);
eq(m.bump(), 41, "thread-local starts from its initializer"); eq(m.bump(), 42, "and keeps state");
let t = 0; for (let i = 0; i < 1e6; i++) t = m.bump(); eq(t, 42 + 1e6, "hot");
const bytes = new Uint8Array([0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39]);
if (onLinux)
    eq(m.bigEndian(bytes), 0x31323334n, "ns_get32 through libresolv found via the libraries table");
else
    eq(m.crc(bytes, 9), 0xcbf43926n, "crc32(\"123456789\") through libz found via the libraries table");

// A library that is not there is an error that names it; so is one named by a path.
for (const name of ["no-such-library-anywhere", "./no/such/library.so", "libno-such-library.so.9"]) {
    let message = "";
    try { $vm.cModule(assemble({ sigs: [], funcs: [], libraries: [name] })); } catch (error) { message = String(error.message); }
    if (!message.startsWith(`cannot load library '${name}': `))
        throw new Error(`loading ${name}: ${message}`);
}
print("tls + libraries ok");
