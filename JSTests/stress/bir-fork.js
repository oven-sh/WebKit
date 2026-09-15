//@ skip if !$isFTLPlatform
//@ skip if $hostOS == "windows"
//@ requireOptions("--useDollarVM=1")

// What a C program forks is a copy of itself: the child goes on running the program's own code, and finds its
// objects there, the constant ones, the written ones and the thread's, with the values they had.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });

// static const int constant = 6; static int written = 5; static _Thread_local int mine = 7;
// int run(int add) { written += add; mine += add; pid_t child = fork(); if (!child) _exit(constant + written + mine);
//                    int status; waitpid(child, &status, 0); return status; }
// int touch(void) { return mine; }
const PART = 16384;
const c = $vm.cModule(assemble({
    sigs: [{ ret: T.i32, params: [] }, { ret: T.void, params: [T.i32] }, { ret: T.i32, params: [T.i32, T.i64, T.i32] }, { ret: T.i32, params: [T.i32] }],
    externs: [{ name: "fork", sig: 0 }, { name: "_exit", sig: 1 }, { name: "waitpid", sig: 2 }],
    data: { size: PART + 4, align: 4, readOnly: PART, constants: [6, 0, 0, 0], init: [5, 0, 0, 0], relocs: [] },
    tls: { size: 4, align: 4, init: [7, 0, 0, 0] },
    funcs: [
        { name: "run", sig: 3, exported: true, locals: [T.i32], slots: [{ size: 4, align: 4 }], blocks: [
            [["DataAddr", PART], ["Load", b(MEM.i32), 1, s(0)], ["Add", 2, 0], ["Store", b(MEM.i32), 3, 1, s(0)],
                ["TlsAddr", 0], ["Load", b(MEM.i32), 4, s(0)], ["Add", 5, 0], ["Store", b(MEM.i32), 6, 4, s(0)],
                ["CallExtern", 0, 0], ["LocalSet", 0, 7], ["ConstI32", s(0)], ["Eq", 7, 8], ["Br", 9, 1, 2]],
            [["DataAddr", 0], ["Load", b(MEM.i32), 10, s(0)], ["DataAddr", PART], ["Load", b(MEM.i32), 12, s(0)], ["Add", 11, 13],
                ["TlsAddr", 0], ["Load", b(MEM.i32), 15, s(0)], ["Add", 14, 16], ["CallExtern", 1, 1, 17], ["Unreachable"]],
            [["SlotAddr", 0], ["LocalGet", 0], ["ConstI32", s(0)], ["CallExtern", 2, 3, 19, 18, 20], ["Load", b(MEM.i32), 18, s(0)], ["Ret", 22]],
        ] },
        { name: "touch", sig: 0, exported: true, blocks: [[["TlsAddr", 0], ["Load", b(MEM.i32), 0, s(0)], ["Ret", 1]]] },
    ],
    exports: [{ name: "run", func: 0, ret: FFI.i32, args: [FFI.i32] }, { name: "touch", func: 1, ret: FFI.i32, args: [] }],
}));

// The status of a child that left through _exit(n) is n << 8; one that a signal killed has the signal's number in it.
eq(c.touch(), 7, "the thread's own object before anything");
eq(c.run(1), (6 + 6 + 8) << 8, "the first child's status");
eq(c.run(10), (6 + 16 + 18) << 8, "the second child's status");
eq(c.touch(), 18, "the parent's objects are its own");
// From code the optimizing JIT has put inside JavaScript's.
function hot(n) { let last = 0; for (let i = 0; i < n; i++) last = c.touch(); return last; }
noInline(hot);
eq(hot(100000), 18, "touch, hot");
eq(c.run(0), (6 + 16 + 18) << 8, "a child forked after all that");
print("fork ok");
