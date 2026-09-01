//@ requireOptions("--useJSThreads=1", "--useSharedGCHeap=1")
// A wasm-GC instance copies the server LocalAllocators of the wasm-GC
// subspace into its cell, and a shared GC heap never materializes them. A
// module that defines a struct or array type must therefore be refused
// gracefully on the main thread: WebAssembly.validate returns false and the
// compile surface throws CompileError, instead of aborting the process at
// instantiation. Non-GC wasm keeps working under the same options (the U17
// negative arm).
load("../harness.js", "caller relative");

if (typeof WebAssembly !== "undefined") {
    const header = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];
    // (type (struct (field i32)))
    const structModuleBytes = new Uint8Array([...header, 0x01, 0x05, 0x01, 0x5f, 0x01, 0x7f, 0x00]);
    // (type (array (mut i32)))
    const arrayModuleBytes = new Uint8Array([...header, 0x01, 0x04, 0x01, 0x5e, 0x7f, 0x01]);
    // (type (func))
    const funcTypeModuleBytes = new Uint8Array([...header, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00]);

    for (const bytes of [structModuleBytes, arrayModuleBytes]) {
        shouldBeFalse(WebAssembly.validate(bytes), "validate refuses a GC-typed module under a shared heap");
        shouldThrow(WebAssembly.CompileError, () => new WebAssembly.Module(bytes));
    }

    shouldBeTrue(WebAssembly.validate(funcTypeModuleBytes), "non-GC wasm still validates");
    shouldNotThrow(() => new WebAssembly.Instance(new WebAssembly.Module(funcTypeModuleBytes)), "non-GC wasm still instantiates");
}
