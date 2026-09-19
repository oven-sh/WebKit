//@ skip if !$isFTLPlatform
//@ skip if $hostOS == "windows"
//@ requireOptions("--useDollarVM=1")

// What a module declares and does not define is looked for in what the embedder defines itself, then in the libraries
// the module names, then in the process. A library answers for the libraries it depends on too, the C library among
// them, so without the first step naming any library would replace the embedder's own atexit or quick_exit with the C
// library's. This shell defines quick_exit itself.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const library = $vm.cModuleHost()[1] === 1 ? "libz.1.dylib" : "libm.so.6";
// void* where(void) { return &<name>; }
const addressOf = (name, libraries) => $vm.cModule(assemble({
    sigs: [{ ret: T.i64, params: [] }, { ret: T.void, params: [T.i32] }],
    externs: [{ name, sig: 1 }],
    funcs: [{ name: "where", sig: 0, exported: true, blocks: [[["ExternAddr", 0], ["Ret", 0]]] }],
    exports: [{ name: "where", func: 0, ret: FFI.u64, args: [] }],
    libraries,
})).where();
const own = addressOf("quick_exit", []);
eq(addressOf("quick_exit", [library]), own, "quick_exit in a module that names a library");
eq(addressOf("quick_exit", [library, library]), own, "quick_exit in a module that names two");
// What the embedder does not define is the library's, or the process's, as before.
const fromTheProcess = addressOf("exit", []);
if (fromTheProcess === own || !fromTheProcess)
    throw new Error("exit is not the shell's own");
eq(addressOf("exit", [library]), fromTheProcess, "exit in a module that names a library");
print("extern search order ok");
