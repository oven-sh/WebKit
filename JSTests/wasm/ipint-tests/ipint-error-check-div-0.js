import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

let wat = `
(module
    (func (export "test") (param i32 i32) (result i32)
        (local.get 0)
        (local.get 1)
        (i32.div_u)
        (return)
    )
)
`

async function test() {
    const instance = await instantiate(wat, {});
    const { test } = instance.exports
    // Bun: a trap's message has no source text. Upstream expects the suffix " (evaluating 'test(1, 0)')".
    assert.throws(() => {test(1, 0)}, WebAssembly.RuntimeError, "Division by zero");
}

await assert.asyncTest(test())
