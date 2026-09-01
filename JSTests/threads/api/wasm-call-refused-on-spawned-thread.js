//@ requireOptions("--useJSThreads=1")
// A spawned Thread calling a WebAssembly export created on the main thread
// must get a TypeError from callWebAssemblyFunction, the only JS->wasm entry
// under useJSThreads (the LLInt js_to_wasm trampoline, the wasm call thunk,
// the JS->wasm IC and the DFG CallWasm conversion are all disabled by the
// flag). Repeated calls keep throwing once the call site is warm, and the
// main thread keeps calling the same export successfully before and after.
load("../harness.js", "caller relative");

if (typeof WebAssembly !== "undefined") {
    // (module (func (export "f") (result i32) i32.const 42))
    const bytes = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00,
        0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b,
    ]);
    const { f } = new WebAssembly.Instance(new WebAssembly.Module(bytes)).exports;
    shouldBe(f(), 42, "carrier call before spawn");

    const iterations = 200;
    const result = new Thread(() => {
        let typeErrors = 0;
        let other = null;
        for (let i = 0; i < iterations; ++i) {
            try {
                f();
                other = other || "no-throw";
            } catch (e) {
                if (e instanceof TypeError)
                    ++typeErrors;
                else
                    other = other || String(e);
            }
        }
        return JSON.stringify({ typeErrors, other });
    }).join();

    const outcomes = JSON.parse(result);
    shouldBe(outcomes.other, null, "spawned-thread wasm call outcome other than TypeError");
    shouldBe(outcomes.typeErrors, iterations, "every spawned-thread wasm call throws TypeError");

    shouldBe(f(), 42, "carrier call after join");
}
