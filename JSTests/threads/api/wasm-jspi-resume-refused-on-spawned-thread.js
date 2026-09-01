//@ requireOptions("--useJSThreads=1")
// WebAssembly is refused on spawned Threads, and that must include JSPI
// resumption: the pinball fulfill/reject handlers that a Suspending import
// attaches to its promise are ordinary promise reactions, so when a spawned
// Thread settles that promise the reaction is drained on the spawned Thread
// (the main thread is parked in join()). The handler must not re-implant the
// evacuated wasm frames there; the promising() result promise rejects with a
// TypeError instead. The same program resumed from the main thread completes.
load("../harness.js", "caller relative");

if (typeof WebAssembly !== "undefined" && typeof WebAssembly.Suspending === "function" && typeof WebAssembly.promising === "function") {
    // (module
    //   (import "m" "imp" (func $imp (result i32)))
    //   (func (export "main") (result i32) call $imp i32.const 1 i32.add))
    const bytes = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x02, 0x09, 0x01, 0x01, 0x6d, 0x03, 0x69, 0x6d, 0x70, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x08, 0x01, 0x04, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x01,
        0x0a, 0x09, 0x01, 0x07, 0x00, 0x10, 0x00, 0x41, 0x01, 0x6a, 0x0b,
    ]);

    function suspendedMain() {
        let settle = null;
        const imp = new WebAssembly.Suspending(() => new Promise((resolve, reject) => { settle = { resolve, reject }; }));
        const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), { m: { imp } });
        const main = WebAssembly.promising(instance.exports.main);
        const outcome = { state: "pending" };
        main().then((value) => { outcome.state = "fulfilled"; outcome.value = value; },
            (error) => { outcome.state = "rejected"; outcome.error = error; });
        shouldBeTrue(settle !== null, "wasm suspended on the Suspending import");
        return { settle, outcome };
    }

    // Carrier resumption is unaffected.
    {
        const { settle, outcome } = suspendedMain();
        settle.resolve(41);
        drainMicrotasks();
        shouldBe(outcome.state, "fulfilled", "carrier-resumed promising() call settles");
        shouldBe(outcome.value, 42, "carrier-resumed promising() call result");
    }

    // Fulfillment from a spawned Thread is refused.
    {
        const { settle, outcome } = suspendedMain();
        new Thread(() => { settle.resolve(41); }).join();
        drainMicrotasks();
        shouldBe(outcome.state, "rejected", "spawned-thread fulfillment rejects the promising() result");
        shouldBeTrue(outcome.error instanceof TypeError, "spawned-thread fulfillment rejection is a TypeError");
    }

    // Rejection from a spawned Thread is refused the same way.
    {
        const { settle, outcome } = suspendedMain();
        new Thread(() => { settle.reject(new Error("import failed")); }).join();
        drainMicrotasks();
        shouldBe(outcome.state, "rejected", "spawned-thread rejection rejects the promising() result");
        shouldBeTrue(outcome.error instanceof TypeError, "spawned-thread rejection refusal is a TypeError");
    }

    // The carrier can still run JSPI afterwards.
    {
        const { settle, outcome } = suspendedMain();
        settle.resolve(9);
        drainMicrotasks();
        shouldBe(outcome.state, "fulfilled", "carrier JSPI still works after the refusals");
        shouldBe(outcome.value, 10, "carrier JSPI result after the refusals");
    }
}
