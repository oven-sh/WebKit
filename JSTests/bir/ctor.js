load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
const m = $vm.cModule(assemble({
    sigs: [{ ret: T.void, params: [] }, { ret: T.i32, params: [] }],
    data: { size: 8, align: 4, init: [], relocs: [] },
    funcs: [
        { name: "init_a", sig: 0, blocks: [[["DataAddr", 0], ["Load", t8(MEM.i32), 0, s(0)], ["ConstI32", s(10)], ["Mul", 1, 2], ["ConstI32", s(1)], ["Add", 3, 4], ["Store", t8(MEM.i32), 5, 0, s(0)], ["RetVoid"]]] },
        { name: "init_b", sig: 0, blocks: [[["DataAddr", 0], ["Load", t8(MEM.i32), 0, s(0)], ["ConstI32", s(10)], ["Mul", 1, 2], ["ConstI32", s(2)], ["Add", 3, 4], ["Store", t8(MEM.i32), 5, 0, s(0)], ["RetVoid"]]] },
        { name: "get", sig: 1, exported: true, blocks: [[["DataAddr", 0], ["Load", t8(MEM.i32), 0, s(0)], ["Ret", 1]]] },
    ],
    exports: [{ name: "get", func: 2, ret: FFI.i32, args: [] }],
    constructors: [0, 1],
}));
eq(m.get(), 12, "constructors ran once each, in order, before the first call");
print("ctor ok");
