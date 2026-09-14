//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// An atomic read-modify-write whose address is a stack slot that many other instructions also address. B3 stops
// folding an address into its users once there are more than ten of them, and lowering an atomic makes a loop:
// what computes the address has to end up in front of the loop.
load("./resources/bir-assembler.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n }), t8 = n => ({ u8: n });
const AND = 2, OR = 3, EXCHANGE = 5, SEQ_CST = 4;

// int f(int start) { short x[13] = { start, ... }; int a = fetch_and(&x[0], 6); int b = fetch_or(&x[0], 1);
//                    int c = exchange(&x[0], 40); return a * 10000 + b * 100 + c + x[0] + (x[1] + ... + x[12]) * 1000000; }
const body = [];
let id = 1; // 0 is the parameter
const slotAddr = () => { body.push(["SlotAddr", 0]); return id++; };
// Thirteen stores that are all needed: to the variable itself and to twelve other places in the same slot.
for (let i = 0; i < 13; i++) body.push(["Store", t8(MEM.i16u), 0, slotAddr(), s(i * 2)]);
const constant = n => { body.push(["ConstI32", s(n)]); return id++; };
const rmw = (op, operand) => { const value = constant(operand); const address = slotAddr(); body.push(["AtomicRmw", t8(op), t8(MEM.i16u), t8(SEQ_CST), value, address]); return id++; };
const a = rmw(AND, 6), b = rmw(OR, 1), c = rmw(EXCHANGE, 40);
body.push(["Load", t8(MEM.i16s), slotAddr(), s(0)]); const x = id++;
let others = null;
for (let i = 1; i < 13; i++) { body.push(["Load", t8(MEM.i16s), slotAddr(), s(i * 2)]); const v = id++; if (others === null) others = v; else { body.push(["Add", others, v]); others = id++; } }
const mul = (v, n) => { const k = constant(n); body.push(["Mul", v, k]); return id++; };
const add = (l, r) => { body.push(["Add", l, r]); return id++; };
body.push(["Ret", add(add(add(add(mul(a, 10000), mul(b, 100)), c), x), mul(others, 1000000))]);

const m = $vm.cModule(assemble({
    sigs: [{ ret: T.i32, params: [T.i32] }],
    funcs: [{ name: "f", sig: 0, exported: true, slots: [{ size: 26, align: 2 }], blocks: [body] }],
    exports: [{ name: "f", func: 0, ret: FFI.i32, args: [FFI.i32] }],
}));
// start 7: and 6 -> old 7, x 6; or 1 -> old 6, x 7; exchange 40 -> old 7, x 40
for (let i = 0; i < 1e4; i++) eq(m.f(7), 7 * 10000 + 6 * 100 + 7 + 40 + 12 * 7 * 1000000, "atomics on a slot with many users");

// A compare-and-swap that stores a local's address into another local, its result only compared. B3 emits such a
// compare-and-swap where the comparison is, and on a target where it is a loop of its own blocks, inside new blocks:
// the addresses have to be computed ahead of those, and the swap must not move past a load of the cell between the two.
//   long g(void) { void* cell = 0; long local; void* old = cas(&cell, 0, &local); void* now = cell; return COMPARE(old, 0) ? now == &local : -1; }
const SEQUENTIALLY_CONSISTENT = 4;
for (const compare of ["Eq", "Ne"]) {
    for (const loadBetween of [false, true]) {
        const entry = new Block(0);
        const zero = entry.def("ConstI64", s(0));
        entry.run("Store", t8(MEM.i64), zero, entry.def("SlotAddr", 0), s(0));
        const old = entry.def("AtomicCas", t8(MEM.i64), t8(SEQUENTIALLY_CONSISTENT), t8(SEQUENTIALLY_CONSISTENT), zero, entry.def("SlotAddr", 1), entry.def("SlotAddr", 0));
        if (loadBetween)
            entry.run("LocalSet", 0, entry.def("Load", t8(MEM.i64), entry.def("SlotAddr", 0), s(0)));
        entry.run("Br", entry.def(compare, old, zero), compare === "Eq" ? 1 : 2, compare === "Eq" ? 2 : 1);
        const swapped = new Block(entry.next);
        const now = loadBetween ? swapped.def("LocalGet", 0) : swapped.def("Load", t8(MEM.i64), swapped.def("SlotAddr", 0), s(0));
        swapped.run("Ret", swapped.def("ZExt32", swapped.def("Eq", now, swapped.def("SlotAddr", 1))));
        const failed = new Block(swapped.next);
        failed.run("Ret", failed.def("ConstI64", s(-1)));
        const g = $vm.cModule(assemble({
            sigs: [{ ret: T.i64, params: [] }],
            funcs: [{ name: "g", sig: 0, exported: true, locals: [T.i64], slots: [{ size: 8, align: 8 }, { size: 8, align: 8 }], blocks: [entry.insts, swapped.insts, failed.insts] }],
            exports: [{ name: "g", func: 0, ret: FFI.i64, args: [] }],
        })).g;
        for (let i = 0; i < 1e4; i++) eq(g(), 1n, `compare-and-swap of a local's address, result compared with ${compare}${loadBetween ? ", the cell loaded in between" : ""}`);
    }
}
print("atomic on a busy slot ok");
