//@ skip if !$isFTLPlatform
//@ skip if $hostOS == "windows"
//@ requireOptions("--useDollarVM=1")

// A fence for the compiler only (C's atomic_signal_fence) is no instruction, and a place no access to memory is moved
// across: a loop that reads an ordinary object around one sees what another thread, or a signal handler, stored.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });
const ORDER = { relaxed: 0, acquire: 1, release: 2, acquireRelease: 3, sequentiallyConsistent: 4 };
const COMPILER = 0x80;

// static int done; static pthread_t thread;
// static void* setter(void* p) { done = 1; return 0; }
// long wait(void) { long n = 0; done = 0; pthread_create(&thread, 0, setter, 0); while (!done) { <the fence>; n++; } pthread_join(thread, 0); return n; }
// long twoLoads(int* p) { int a = *p; <the fence>; int b = *p; return a + b; }        compiles to two loads, which nothing here can tell
const DONE = 0, THREAD = 8;
const waitWith = order => [
    [["DataAddr", DONE], ["ConstI32", s(0)], ["Store", b(MEM.i32), 1, 0, s(0)],
        ["DataAddr", THREAD], ["ConstI64", s(0)], ["FuncAddr", 0], ["CallExtern", 0, 4, 2, 3, 4, 3], ["Jump", 1]],
    [["Fence", b(order)], ["LocalGet", 0], ["ConstI64", s(1)], ["Add", 6, 7], ["LocalSet", 0, 8],
        ["DataAddr", DONE], ["Load", b(MEM.i32), 9, s(0)], ["Br", 10, 2, 1]],
    [["DataAddr", THREAD], ["Load", b(MEM.i64), 11, s(0)], ["ConstI64", s(0)], ["CallExtern", 1, 2, 12, 13], ["LocalGet", 0], ["Ret", 15]],
];
const orders = Object.entries(ORDER).filter(([name]) => name !== "relaxed");
const funcs = [{ name: "setter", sig: 0, blocks: [[["DataAddr", DONE], ["ConstI32", s(1)], ["Store", b(MEM.i32), 2, 1, s(0)], ["ConstI64", s(0)], ["Ret", 3]]] }];
const exports = [];
for (const [name, order] of orders) {
    funcs.push({ name: "wait_" + name, sig: 1, exported: true, locals: [T.i64], blocks: waitWith(order | COMPILER) });
    exports.push({ name: "wait_" + name, func: funcs.length - 1, ret: FFI.i64, args: [] });
}
// A relaxed one is nothing at all, whoever it is for: it only has to load.
funcs.push({ name: "relaxed", sig: 1, exported: true, blocks: [[["Fence", b(ORDER.relaxed | COMPILER)], ["Fence", b(ORDER.relaxed)], ["ConstI64", s(7)], ["Ret", 0]]] });
exports.push({ name: "relaxed", func: funcs.length - 1, ret: FFI.i64, args: [] });
const c = $vm.cModule(assemble({
    sigs: [{ ret: T.i64, params: [T.i64] }, { ret: T.i64, params: [] }, { ret: T.i32, params: [T.i64, T.i64, T.i64, T.i64] }, { ret: T.i32, params: [T.i64, T.i64] }],
    externs: [{ name: "pthread_create", sig: 2 }, { name: "pthread_join", sig: 3 }],
    data: { size: 16, align: 8, init: [], relocs: [] },
    funcs, exports,
}));
for (const [name] of orders) {
    for (let round = 0; round < 5; round++) {
        if (!(c["wait_" + name]() >= 1n))
            throw new Error(`a compiler fence of order ${name}: the loop did not run`);
    }
}
eq(c.relaxed(), 7n, "relaxed fences");
print("compiler fence ok");
