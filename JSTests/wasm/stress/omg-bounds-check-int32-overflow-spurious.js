//@ requireOptions("--useWasmFastMemory=false")
// Regression: in BoundsChecking mode, OMG's WasmBoundsCheck overflow guard did
// a 64-bit compare of (zext32(ptr)+offset) against the raw Int32 ptr Tmp, whose
// upper bits are undefined. If the i32 arg arrives with dirty upper bits (e.g.
// passed from IPInt where i32.wrap_i64 is a no-op), the guard fires spuriously.

import { instantiate } from "../wabt-wrapper.js";
import * as assert from "../assert.js";

async function test() {
    const instance = await instantiate(`
        (module
          (memory 1)
          (func $load (export "load") (param i32) (result i32)
            (i32.load offset=4 (local.get 0)))
          (func (export "run") (param i64) (result i32)
            ;; wrap leaves the 64-bit operand on IPInt's stack untouched, so the
            ;; i32 arg passed to $load carries the original high bits.
            (call $load (i32.wrap_i64 (local.get 0)))))
    `, {});

    // High dword set to all-ones; low dword is a valid in-bounds address (0).
    const dirty = 0xffffffff00000000n;
    for (let i = 0; i < 100000; ++i)
        assert.eq(instance.exports.run(dirty), 0);
}

await assert.asyncTest(test());
