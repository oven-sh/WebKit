//@ skip if !$isFTLPlatform
//@ skip if $hostOS == "windows"
//@ requireOptions("--useDollarVM=1")

// A loaded C module stays loaded for as long as the process lives, whether or not JavaScript still holds any
// of its functions: C hands out pointers into itself that nothing counts. Here a module registers a function
// of its own to run at exit and another as a signal handler, and keeps the address of one of its statics in
// memory JavaScript owns; then every reference to it is dropped, the heap is collected, and other modules are
// compiled over where its code and data would have been.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });
const [arch, os] = $vm.cModuleHost();
const SIGUSR1 = os === 0 ? 10 : 30;

// static int* cell; static int counter = 41;
// static void atExit(void* unused) { *cell += 1000; counter++; }
// static void onSignal(int signal) { *cell += signal; counter++; }
// __attribute__((constructor)) static void registerAtExit(void) { __cxa_atexit(atExit, 0, 0); }
// int* install(int* out, int signal) { cell = out; signal(signal, onSignal); return &counter; }
function moduleThatRegistersItself() {
    return {
        sigs: [{ ret: T.void, params: [T.i64] }, { ret: T.void, params: [T.i32] }, { ret: T.void, params: [] }, { ret: T.i64, params: [T.i64, T.i32] },
            { ret: T.i32, params: [T.i64, T.i64, T.i64] }, { ret: T.i64, params: [T.i32, T.i64] }],
        externs: [{ name: "__cxa_atexit", sig: 4 }, { name: "signal", sig: 5 }],
        data: { size: 16, align: 8, init: [0, 0, 0, 0, 0, 0, 0, 0, 41, 0, 0, 0], relocs: [] },
        funcs: [
            { name: "atExit", sig: 0, blocks: [[["DataAddr", 0], ["Load", b(MEM.i64), 1, s(0)], ["Load", b(MEM.i32), 2, s(0)], ["ConstI32", s(1000)], ["Add", 3, 4], ["Store", b(MEM.i32), 5, 2, s(0)],
                ["DataAddr", 8], ["Load", b(MEM.i32), 6, s(0)], ["ConstI32", s(1)], ["Add", 7, 8], ["Store", b(MEM.i32), 9, 6, s(0)], ["RetVoid"]]] },
            { name: "onSignal", sig: 1, blocks: [[["DataAddr", 0], ["Load", b(MEM.i64), 1, s(0)], ["Load", b(MEM.i32), 2, s(0)], ["Add", 3, 0], ["Store", b(MEM.i32), 4, 2, s(0)],
                ["DataAddr", 8], ["Load", b(MEM.i32), 5, s(0)], ["ConstI32", s(1)], ["Add", 6, 7], ["Store", b(MEM.i32), 8, 5, s(0)], ["RetVoid"]]] },
            { name: "registerAtExit", sig: 2, blocks: [[["FuncAddr", 0], ["ConstI64", s(0)], ["CallExtern", 0, 3, 0, 1, 1], ["RetVoid"]]] },
            { name: "install", sig: 3, exported: true, blocks: [[["DataAddr", 0], ["Store", b(MEM.i64), 0, 2, s(0)], ["FuncAddr", 1], ["CallExtern", 1, 2, 1, 3], ["DataAddr", 8], ["Ret", 5]]] },
        ],
        exports: [{ name: "install", func: 3, ret: FFI.ptr, args: [FFI.ptr, FFI.i32] }],
        constructors: [2],
    };
}
// int raise_(int signal) { return raise(signal); }     int peek(const int* p) { return *p; }     and some code to take up room
function anotherModule(salt) {
    const filler = new Block(1);
    let x = 0;
    for (let i = 0; i < 60; i++)
        x = filler.def("Xor", filler.def("Mul", x, filler.def("ConstI32", s(salt + i))), filler.def("ConstI32", s(i)));
    filler.run("Ret", x);
    return {
        sigs: [{ ret: T.i32, params: [T.i32] }, { ret: T.i32, params: [T.i64] }],
        externs: [{ name: "raise", sig: 0 }],
        data: { size: 65536, align: 8, init: new Array(64).fill(0xcc), relocs: [] },
        funcs: [
            { name: "raise_", sig: 0, exported: true, blocks: [[["CallExtern", 0, 1, 0], ["Ret", 1]]] },
            { name: "peek", sig: 1, exported: true, blocks: [[["Load", b(MEM.i32), 0, s(0)], ["Ret", 1]]] },
            { name: "filler", sig: 0, exported: true, blocks: [filler.insts] },
        ],
        exports: [{ name: "raise_", func: 0, ret: FFI.i32, args: [FFI.i32] }, { name: "peek", func: 1, ret: FFI.i32, args: [FFI.ptr] }, { name: "filler", func: 2, ret: FFI.i32, args: [FFI.i32] }],
    };
}

const cell = new Int32Array(1);
// Nothing of the module is referred to after this call returns.
const counterAddress = (function() { return $vm.cModule(assemble(moduleThatRegistersItself())).install(cell, SIGUSR1); })();
let other;
for (let round = 0; round < 8; round++) {
    gc();
    for (let i = 0; i < 40; i++) {
        other = $vm.cModule(assemble(anotherModule(round * 100 + i)));
        other.filler(i);
    }
}
gc();
eq(other.peek(counterAddress), 41, "the module's static is still there");
eq(other.raise_(SIGUSR1), 0, "raise");
eq(cell[0], SIGUSR1, "the module's signal handler ran");
eq(other.peek(counterAddress), 42, "and counted");
eq(other.raise_(SIGUSR1), 0, "raise again");
eq(other.peek(counterAddress), 43, "and again");
// Its exit handler runs after this script ends; the process exits normally only if it is still there to run.
print("module lifetime ok");
