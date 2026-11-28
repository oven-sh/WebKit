//@ requireOptions("--forceICFailure=1")
import * as assert from "../assert.js";

/*
    (module
        (import "wasm:js-string" "compare" (func $compare (param externref externref) (result i32)))
        (func (export "loop") (param $left externref) (param $right externref) (param $count i32)
            (loop $loop
                (drop (call $compare (local.get $left) (local.get $right)))
                (br_if $loop (local.tee $count (i32.sub (local.get $count) (i32.const 1)))))))
*/
const bytes = Uint8Array.fromBase64("AGFzbQEAAAABDQJgAm9vAX9gA29vfwACGgEOd2FzbTpqcy1zdHJpbmcHY29tcGFyZQAAAwIBAQcIAQRsb29wAAEKFwEVAANAIAAgARAAGiACQQFrIgINAAsL");
const { loop } = new WebAssembly.Instance(new WebAssembly.Module(bytes, { builtins: ["js-string"] }), { }).exports;

function caller(left, right, count) {
    loop(left, right, count);
}
noInline(caller);

function checkError() {
    let error;
    try {
        caller(1, 2, 1);
    } catch (thrown) {
        error = thrown;
    }
    assert.eq(error.message, "Illegal argument (evaluating 'loop(left, right, count)')");
    assert.truthy(/^1@wasm-function\[1\]\ncaller@/.test(error.stack), error.stack);
}
checkError();
for (let i = 0; i < wasmTestLoopCount; i++)
    caller("a", "b", 2);
checkError();

if (platformSupportsSamplingProfiler()) {
    const [left, right] = ["b", "c"].map(last => ["a".repeat(20000), last].join(""));
    const startTime = Date.now();
    // A thread that is kept from running collects samples wherever it happens to be, so one batch proves nothing wrong.
    for (let inBuiltin = 0, total = 0; total < 40 || inBuiltin < 0.8 * total;) {
        if (Date.now() - startTime > 50000)
            throw new Error(`${inBuiltin} of ${total}`);
        if (total >= 40)
            inBuiltin = total = 0;
        startSamplingProfiler({ intervalInMicroseconds: 100, recordsSamplesWithoutFrames: true });
        for (const end = preciseTime() + 0.002; preciseTime() < end;)
            caller(left, right, 50);
        pauseSamplingProfiler();
        for (const { frames } of releaseSamplingProfilerSamples().stackTraces) {
            inBuiltin += frames.join().startsWith(".wasm-function[wasm:js-string.compare],.wasm-function[1],wasm-stub,caller,");
            total++;
        }
    }
}
