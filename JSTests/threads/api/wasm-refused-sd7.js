//@ requireOptions("--useJSThreads=1")
// SD7 (SPEC-ungil §I, NORMATIVE in BOTH GIL modes): WebAssembly is REFUSED
// on spawned JS Threads in v1 — the ctor/compile surface throws TypeError on
// a spawned thread. This exercises the C++ gate
// (JSWebAssemblyHelpers.h throwIfWebAssemblyRefusedOnSpawnedThread); the
// generated-code arm (JSToWasm prologue for WARM calls of carrier-created
// exports) is AB-15 (docs/threads/INTEGRATE-ungil.md) and is not covered
// here. The carrier-side negative arm (U17): the same surface does NOT
// throw on the main thread.
load("../harness.js", "caller relative");

if (typeof WebAssembly !== "undefined") {
    // Smallest valid module: just the magic + version header.
    const emptyModuleBytes = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]);

    // U17 negative arm: carrier (main thread) wasm never throws the SD7 gate.
    shouldNotThrow(() => new WebAssembly.Module(emptyModuleBytes));
    shouldNotThrow(() => new WebAssembly.Memory({ initial: 1 }));
    shouldBeTrue(WebAssembly.validate(emptyModuleBytes));

    // SD7 positive arm: every ctor/compile entry point throws TypeError on a
    // spawned thread. Run the probes inside the thread, collect outcomes,
    // and assert on the joined result so harness assertions stay on main.
    const result = new Thread(() => {
        const probes = {
            module: () => new WebAssembly.Module(emptyModuleBytes),
            memory: () => new WebAssembly.Memory({ initial: 1 }),
            table: () => new WebAssembly.Table({ element: "funcref", initial: 0 }),
            global: () => new WebAssembly.Global({ value: "i32" }, 0),
            tag: () => new WebAssembly.Tag({ parameters: [] }),
            validate: () => WebAssembly.validate(emptyModuleBytes),
            compile: () => WebAssembly.compile(emptyModuleBytes),
            instantiate: () => WebAssembly.instantiate(emptyModuleBytes),
        };
        const outcomes = {};
        for (const name in probes) {
            try {
                probes[name]();
                outcomes[name] = "no-throw";
            } catch (e) {
                outcomes[name] = e instanceof TypeError ? "TypeError" : String(e);
            }
        }
        return JSON.stringify(outcomes);
    }).join();

    const outcomes = JSON.parse(result);
    for (const name in outcomes)
        shouldBe(outcomes[name], "TypeError", `SD7: WebAssembly ${name} on a spawned thread`);
}
