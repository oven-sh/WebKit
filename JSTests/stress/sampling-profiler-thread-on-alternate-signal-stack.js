//@ requireOptions("--useSamplingProfiler=true", "--sampleInterval=50")

// jsc creates the thread of an agent with WTF::Thread::create(), so that thread has an alternate
// signal stack, and the signal handlers of JSC run on it: the one that turns a fault in Wasm code
// into a trap, and the one that jettisons the code a VM trap stopped in. Both wait for locks that
// the sampling profiler holds while it suspends the thread. If the profiler cannot suspend a thread
// that runs on its alternate signal stack, this test does not finish.

const numberOfTraps = 20000;
const numberOfTimeLimits = 30;

$.agent.start(`
    function takeWasmTraps()
    {
        if (typeof WebAssembly !== "object")
            return ${numberOfTraps};

        // (module (memory 1) (func (export "load") (param i32) (result i32) (i32.load (local.get 0))))
        const module = new WebAssembly.Module(new Uint8Array([
            0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
            0x03, 0x02, 0x01, 0x00,
            0x05, 0x03, 0x01, 0x00, 0x01,
            0x07, 0x08, 0x01, 0x04, 0x6c, 0x6f, 0x61, 0x64, 0x00, 0x00,
            0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,
        ]));
        const { load } = new WebAssembly.Instance(module).exports;

        let traps = 0;
        for (let i = 0; i < ${numberOfTraps}; ++i) {
            try {
                load(0x7fff0000);
            } catch (error) {
                if (!(error instanceof WebAssembly.RuntimeError))
                    throw error;
                ++traps;
            }
        }
        return traps;
    }

    function runIntoTimeLimits()
    {
        let timeLimits = 0;
        for (let i = 0; i < ${numberOfTimeLimits}; ++i) {
            try {
                $vm.callWithTimeLimit(() => { for (;;) { } }, 5);
            } catch (error) {
                if (!(error instanceof RangeError) || error.message !== "timed out")
                    throw error;
                ++timeLimits;
            }
        }
        return timeLimits;
    }

    let report;
    try {
        report = "traps: " + takeWasmTraps() + ", time limits: " + runIntoTimeLimits();
    } catch (error) {
        report = "error: " + error;
    }
    $.agent.report(report);
`);

let report;
while (!(report = $.agent.getReport()))
    $.agent.sleep(1);

const expected = "traps: " + numberOfTraps + ", time limits: " + numberOfTimeLimits;
if (report !== expected)
    throw new Error("expected '" + expected + "' but the agent reported '" + report + "'");
