//@ requireOptions("--useJSPI=1")
import * as assert from "../assert.js";

// An error raised under a wasm frame (a trap, a SuspendError) has no JS expression of its own. Its message is the fixed
// text, with no " (evaluating '<source text>')" of a JS caller. See JSTests/BUN-TEST-DIFFERENCES.md, A10.

// (module
//   (import "m" "suspending" (func $suspending))
//   (func (export "div0") i32.const 1 i32.const 0 i32.div_s drop)
//   (func (export "unreachable") unreachable)
//   (func (export "callSuspending") call $suspending))
const bytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x02, 0x10, 0x01, 0x01, 0x6d, 0x0a, 0x73, 0x75, 0x73, 0x70, 0x65, 0x6e, 0x64, 0x69, 0x6e, 0x67, 0x00, 0x00,
    0x03, 0x04, 0x03, 0x00, 0x00, 0x00,
    0x07, 0x27, 0x03,
    0x04, 0x64, 0x69, 0x76, 0x30, 0x00, 0x01,
    0x0b, 0x75, 0x6e, 0x72, 0x65, 0x61, 0x63, 0x68, 0x61, 0x62, 0x6c, 0x65, 0x00, 0x02,
    0x0e, 0x63, 0x61, 0x6c, 0x6c, 0x53, 0x75, 0x73, 0x70, 0x65, 0x6e, 0x64, 0x69, 0x6e, 0x67, 0x00, 0x03,
    0x0a, 0x13, 0x03,
    0x08, 0x00, 0x41, 0x01, 0x41, 0x00, 0x6d, 0x1a, 0x0b,
    0x03, 0x00, 0x00, 0x0b,
    0x04, 0x00, 0x10, 0x00, 0x0b,
]);
const suspending = new WebAssembly.Suspending(async () => { });
const { div0, unreachable, callSuspending } = new WebAssembly.Instance(new WebAssembly.Module(bytes), { m: { suspending } }).exports;

function thrownBy(fn) {
    try {
        fn();
    } catch (e) {
        return e;
    }
    throw new Error("did not throw");
}

// In a module a call in tail position leaves no frame: the nearest JS expression is the `fn()` in thrownBy.
const tailCall = thrownBy(() => div0());
assert.instanceof(tailCall, WebAssembly.RuntimeError);
assert.eq(tailCall.message, "Division by zero");
assert.eq(thrownBy(() => { div0(); }).message, "Division by zero");
assert.eq(thrownBy(div0).message, "Division by zero");
assert.eq(thrownBy(() => unreachable()).message, "Unreachable code should not be executed");

const suspendError = thrownBy(() => callSuspending());
assert.instanceof(suspendError, WebAssembly.SuspendError);
assert.eq(suspendError.message, "Suspending() wrapper called outside of a promising() context");
