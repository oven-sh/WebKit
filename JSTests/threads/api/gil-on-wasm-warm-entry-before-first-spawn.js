//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOn
// SPEC-ungil §I, tenth round. With the GIL on, a process that has not spawned
// a Thread calls a WebAssembly export through the warm JS->wasm entry, as on
// main (before: every call went through callWebAssemblyFunction, 30 times
// slower, and the sampling profiler saw no wasm frames - JSTests/stress/
// sampling-profiler-wasm*.js with the flag on). The entry's prologue tests a
// process byte that the first spawn sets, so call sites that were linked to it
// go to the cold entry from then on: the refusal for spawned threads lives
// there. This test links the warm entry from every tier on the main thread
// first, then spawns, and checks that
//  - the spawned thread gets a TypeError from the very same (warm-linked) call
//    sites, every time, including once those sites are hot on that thread;
//  - the main thread keeps getting right answers from the same sites after the
//    spawn (now through the cold entry), with arguments that the warm entry
//    converts inline (int32, double) and ones it sends to its own slow path.
load("../harness.js", "caller relative");

if (typeof WebAssembly !== "undefined") {
    // (module
    //   (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add)
    //   (func (export "half") (param f64) (result f64) local.get 0 f64.const 0.5 f64.mul))
    const bytes = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x0c, 0x02, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7c, 0x01, 0x7c,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x07, 0x0e, 0x02, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x04, 0x68, 0x61, 0x6c, 0x66, 0x00, 0x01,
        0x0a, 0x18, 0x02,
        0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b,
        0x0e, 0x00, 0x20, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x3f, 0xa2, 0x0b,
    ]);
    const { add, half } = new WebAssembly.Instance(new WebAssembly.Module(bytes)).exports;

    // One call site each, shared by both threads.
    function callAdd(a, b) { return add(a, b); }
    function callHalf(x) { return half(x); }
    noInline(callAdd);
    noInline(callHalf);

    function exercise(who, rounds) {
        let sum = 0;
        for (let i = 0; i < rounds; ++i) {
            const s = callAdd(i, 1);
            if (s !== i + 1)
                throw new Error(who + ": add(" + i + ", 1) = " + s);
            const h = callHalf(i + 0.5);
            if (h !== (i + 0.5) / 2)
                throw new Error(who + ": half(" + (i + 0.5) + ") = " + h);
            sum += s;
        }
        // Arguments the warm entry does not convert inline: they take its slow path to the cold entry.
        if (callAdd("40", { valueOf() { return 2; } }) !== 42)
            throw new Error(who + ": add with coerced arguments");
        if (callHalf("9") !== 4.5)
            throw new Error(who + ": half with a string");
        return sum;
    }

    // Before any spawn: every tier links the warm entry.
    const ROUNDS = 40000;
    exercise("main before the first spawn", ROUNDS);

    const verdict = new Thread(() => {
        let typeErrors = 0;
        let other = null;
        const calls = 3000; // hot enough for the spawned thread's executions of the shared sites to be in optimized code
        for (let i = 0; i < calls; ++i) {
            for (const call of [() => callAdd(i, 1), () => callHalf(i + 0.5)]) {
                try {
                    call();
                    other = other || "no exception";
                } catch (e) {
                    if (e instanceof TypeError)
                        ++typeErrors;
                    else
                        other = other || String(e);
                }
            }
        }
        return JSON.stringify({ typeErrors, other, calls });
    }).join();
    const outcome = JSON.parse(verdict);
    shouldBe(outcome.other, null, "a spawned thread's call through a warm-linked site");
    shouldBe(outcome.typeErrors, 2 * outcome.calls, "every spawned-thread wasm call throws TypeError");

    // After the spawn the same sites still answer on the main thread.
    exercise("main after a spawn", ROUNDS);
}
