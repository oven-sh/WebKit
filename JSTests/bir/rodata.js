// The constant part of a module's data: readable, addressable like the rest, and what follows it writable.
load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n }), t8 = n => ({ u8: n });
const PART = 16384; // what the format says the writable part is aligned to
const init = new Array(PART + 8).fill(0);
init[0] = 41; init[PART] = 1;
const m = $vm.cModule(assemble({
    sigs: [{ ret: T.i32, params: [] }, { ret: T.i32, params: [T.i32] }],
    data: { size: PART + 8, align: 16, readOnly: PART, init, relocs: [] },
    funcs: [
        // int constant(void) { return *(const int *)&data[0]; }
        { name: "constant", sig: 0, exported: true, blocks: [[["DataAddr", s(0)], ["Load", t8(MEM.i32), 0, s(0)], ["Ret", 1]]] },
        // int bump(int by) { int *p = (int *)&data[PART]; *p += by; return *p; }
        { name: "bump", sig: 1, exported: true, blocks: [[["DataAddr", s(PART)], ["Load", t8(MEM.i32), 1, s(0)], ["Add", 2, 0], ["Store", t8(MEM.i32), 3, 1, s(0)], ["Ret", 3]]] },
    ],
    exports: [{ name: "constant", func: 0, ret: FFI.i32, args: [] }, { name: "bump", func: 1, ret: FFI.i32, args: [FFI.i32] }],
}));
eq(m.constant(), 41, "reading the constant part");
eq(m.bump(5), 6, "writing after it");
eq(m.bump(10), 16, "writing after it again");
print("rodata ok");
