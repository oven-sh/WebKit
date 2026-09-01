//@ requireOptions("--useJSThreads=1")
// MC-DF S1 (docs/threads/cve/map-MC-DF.md): the CVE-2017-5116 shape.
// In Chrome 61, wasm bytes in a SharedArrayBuffer were validated on one read
// and compiled from another while a Worker rewrote them. Our defense is
// copy-once: every wasm entry point (WebAssemblyModuleConstructor.cpp:301,
// JSWebAssembly.cpp:155/281/422) snapshots the BufferSource into a private
// Vector<uint8_t> via createSourceBufferFromValue BEFORE any validation;
// the validator and all compile tiers consume only the copy.
//
// This test is a TRIPWIRE for that property: it stays green as long as the
// copy stands, and turns into a type-confusion detector the day anyone
// lands a zero-copy "optimization". A spawned thread (wasm itself is
// SD7-refused there, but plain TA writes are not) flips one immediate byte
// between two VALID encodings while main compiles+runs in a loop.
//
// Oracle: every compile either throws CompileError (torn LEB is possible
// and fine — the COPY can be torn, the consumer of the copy is coherent) or
// yields a module whose exported f() returns 1 or 2. Any other result, or a
// crash in the parser/compiler, is the CVE-2017-5116 analog firing.
//
// EXECUTED POST-UNGIL ONLY. Amplifier-ready (nondeterministic interleaving;
// deterministic oracle).
load("../harness.js", "caller relative");

// FIXME(U-T13/MC-LIFE-S6): this premise-skip self-retires when the GIL-off
// wasm refusal is lifted (relocating-grow stop conduction lands); the guard
// below then never fires and the test runs at full strength.
// Wasm is deliberately refused GIL-off (U-T13: 'JSC: disabling useWasm under
// GIL-off...') until the MC-LIFE S6 stop conduction lands. That refusal is
// the accepted engine behavior, not a failure of this tripwire: report the
// runner-recognized premise-skip marker (Tools/threads/run-tests.sh counts
// it as SKIP, never PASS) and exit 0.
if (typeof WebAssembly === "undefined") {
    print("THREADS-PREMISE-SKIP: WebAssembly is unavailable in the effective"
        + " configuration (deliberate U-T13 GIL-off wasm refusal); this"
        + " wasm-class tripwire cannot run meaningfully without it.");
    quit();
}

// (module (func (export "f") (result i32) i32.const <X>))
const moduleBytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic + version
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,       // type: () -> i32
    0x03, 0x02, 0x01, 0x00,                         // func section
    0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00,       // export "f"
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x01, 0x0b, // code: i32.const 1
]);
const IMM_OFFSET = moduleBytes.length - 2; // the i32.const immediate
shouldBe(moduleBytes[IMM_OFFSET], 0x01);

const gate = { started: 0, stop: 0, bytes: moduleBytes };

const flipper = spawnN(1, () => {
    Atomics.add(gate, "started", 1);
    const bytes = gate.bytes;
    const off = bytes.length - 2;
    let flips = 0;
    while (Atomics.load(gate, "stop") === 0) {
        bytes[off] = 0x01 + (flips & 1); // 1 <-> 2, both valid immediates
        flips++;
    }
    return flips;
});

waitUntil(() => Atomics.load(gate, "started") === 1);

const ROUNDS = 500;
for (let r = 0; r < ROUNDS; ++r) {
    try {
        const mod = new WebAssembly.Module(gate.bytes); // main thread: copy-once entry
        const inst = new WebAssembly.Instance(mod);
        const v = inst.exports.f();
        if (v !== 1 && v !== 2)
            throw new Error("compiled module returned " + v + " — validated bytes != compiled bytes");
        // validate() exercises the same copy on a second entry point.
        WebAssembly.validate(gate.bytes);
    } catch (e) {
        if (!(e instanceof WebAssembly.CompileError))
            throw e; // CompileError on a torn copy is acceptable; anything else is not
    }
}
Atomics.store(gate, "stop", 1);

const [flips] = joinAll(flipper);
shouldBeTrue(flips > 0, "flipper made progress");
