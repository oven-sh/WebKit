//@ skip if !$isFTLPlatform
//@ skip if $hostOS == "windows"
//@ requireOptions("--useDollarVM=1")

// A thread's `_Thread_local` objects are still there, with the thread's values in them, when the destructors the
// program registered with pthread_key_create run at the thread's exit, in whichever order the keys were made, for
// every round of destructors but the last; and in the last one too for a key made before the first thread-local
// object of the process was used.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });

// static _Thread_local int counter = 41;
// static pthread_key_t key1, key2; static int seen1, seen2, calls2; static pthread_t thread;
// static void destructor1(void* p) { seen1 = ++counter; }
// static void destructor2(void* p) { seen2 = counter += 10; if (++calls2 < limit) pthread_setspecific(key2, p); }   `limit` rounds
// static void* worker(void* p) { counter = 100; pthread_setspecific(key1, &key1); pthread_setspecific(key2, &key2); return 0; }
// int run(int touchFirst) { if (touchFirst) (void)counter; pthread_key_create(&key1, destructor1); pthread_key_create(&key2, destructor2);
//                           pthread_create(&thread, 0, worker, 0); pthread_join(thread, 0); return seen1; }
// int seen2_(void) { return seen2; }     int mine(void) { return counter; }     int setLimit(int n) { return limit = n; }
const KEY1 = 0, KEY2 = 8, SEEN1 = 16, SEEN2 = 20, CALLS2 = 24, LIMIT = 28, THREAD = 32;
const module = assemble({
    sigs: [{ ret: T.void, params: [T.i64] }, { ret: T.i64, params: [T.i64] }, { ret: T.i32, params: [T.i32] }, { ret: T.i32, params: [] },
        { ret: T.i32, params: [T.i64, T.i64] }, { ret: T.i32, params: [T.i64, T.i64, T.i64, T.i64] }],
    externs: [{ name: "pthread_key_create", sig: 4 }, { name: "pthread_setspecific", sig: 4 }, { name: "pthread_create", sig: 5 }, { name: "pthread_join", sig: 4 }],
    data: { size: 40, align: 8, init: [], relocs: [] },
    tls: { size: 4, align: 4, init: [41, 0, 0, 0] },
    funcs: [
        { name: "destructor1", sig: 0, blocks: [[["TlsAddr", 0], ["Load", b(MEM.i32), 1, s(0)], ["ConstI32", s(1)], ["Add", 2, 3], ["Store", b(MEM.i32), 4, 1, s(0)], ["DataAddr", SEEN1], ["Store", b(MEM.i32), 4, 5, s(0)], ["RetVoid"]]] },
        { name: "destructor2", sig: 0, blocks: [
            [["TlsAddr", 0], ["Load", b(MEM.i32), 1, s(0)], ["ConstI32", s(10)], ["Add", 2, 3], ["Store", b(MEM.i32), 4, 1, s(0)], ["DataAddr", SEEN2], ["Store", b(MEM.i32), 4, 5, s(0)],
             ["DataAddr", CALLS2], ["Load", b(MEM.i32), 6, s(0)], ["ConstI32", s(1)], ["Add", 7, 8], ["Store", b(MEM.i32), 9, 6, s(0)], ["DataAddr", LIMIT], ["Load", b(MEM.i32), 10, s(0)], ["Lt", 9, 11], ["Br", 12, 1, 2]],
            [["DataAddr", KEY2], ["Load", b(MEM.i64), 13, s(0)], ["CallExtern", 1, 2, 14, 0], ["RetVoid"]],
            [["RetVoid"]],
        ] },
        { name: "worker", sig: 1, blocks: [[["TlsAddr", 0], ["ConstI32", s(100)], ["Store", b(MEM.i32), 2, 1, s(0)],
            ["DataAddr", KEY1], ["Load", b(MEM.i64), 3, s(0)], ["CallExtern", 1, 2, 4, 3], ["DataAddr", KEY2], ["Load", b(MEM.i64), 6, s(0)], ["CallExtern", 1, 2, 7, 6], ["ConstI64", s(0)], ["Ret", 9]]] },
        { name: "run", sig: 2, exported: true, blocks: [
            [["Br", 0, 1, 2]],
            [["TlsAddr", 0], ["Load", b(MEM.i32 | 0x80), 1, s(0)], ["Jump", 2]],
            [["DataAddr", KEY1], ["FuncAddr", 0], ["CallExtern", 0, 2, 3, 4], ["DataAddr", KEY2], ["FuncAddr", 1], ["CallExtern", 0, 2, 6, 7],
             ["DataAddr", THREAD], ["ConstI64", s(0)], ["FuncAddr", 2], ["CallExtern", 2, 4, 9, 10, 11, 10], ["Load", b(MEM.i64), 9, s(0)], ["CallExtern", 3, 2, 13, 10],
             ["DataAddr", SEEN1], ["Load", b(MEM.i32), 15, s(0)], ["Ret", 16]],
        ] },
        { name: "seen2", sig: 3, exported: true, blocks: [[["DataAddr", SEEN2], ["Load", b(MEM.i32), 0, s(0)], ["Ret", 1]]] },
        { name: "mine", sig: 3, exported: true, blocks: [[["TlsAddr", 0], ["Load", b(MEM.i32), 0, s(0)], ["Ret", 1]]] },
        { name: "setLimit", sig: 2, exported: true, blocks: [[["DataAddr", LIMIT], ["Store", b(MEM.i32), 0, 1, s(0)], ["Ret", 0]]] },
    ],
    exports: [{ name: "run", func: 3, ret: FFI.i32, args: [FFI.i32] }, { name: "seen2", func: 4, ret: FFI.i32, args: [] }, { name: "mine", func: 5, ret: FFI.i32, args: [] },
        { name: "setLimit", func: 6, ret: FFI.i32, args: [FFI.i32] }],
});

// The system runs four rounds of destructors (PTHREAD_DESTRUCTOR_ITERATIONS). The loader's own key is made the first
// time a thread-local object is used in the process, which is in the thread below: after this module's two keys, so
// in every round its destructor runs after theirs, the last round included. (The order the system takes the keys in
// within a round is glibc's here: the order they were made in.)
const isLinux = $vm.cModuleHost()[1] === 0;
if (isLinux) {
    const m = $vm.cModule(module);
    m.setLimit(4);
    eq(m.run(0), 101, "every key made before the loader's: what the first destructor sees");
    eq(m.seen2(), 141, "every key made before the loader's: what a destructor sees on its fourth and last round");
    eq(m.mine(), 41, "every key made before the loader's: the main thread's own copy");
}
// From here on every new key is made after the loader's. In the last round the thread's copy is gone by the time
// such a key's destructor runs (the C library has no later moment to free it at): what the destructor reads is a
// new copy with the initial value in it.
if (isLinux) {
    const m = $vm.cModule(module);
    m.setLimit(4);
    eq(m.run(0), 101, "keys made after the loader's: what the first destructor sees");
    eq(m.seen2(), 51, "keys made after the loader's: what a destructor sees on its fourth and last round");
}
for (const touchFirst of [0, 1]) {
    for (let round = 0; round < 3; round++) {
        const m = $vm.cModule(module);
        m.setLimit(3);
        const order = touchFirst ? "thread-local data used before the keys are made" : "keys made before thread-local data is used";
        eq(m.run(touchFirst), 101, `${order}: what the first destructor sees`);
        eq(m.seen2(), 131, `${order}: what a destructor sees on its third round`);
        eq(m.mine(), 41, `${order}: the main thread's own copy`);
    }
}
print("tls at thread exit ok");
