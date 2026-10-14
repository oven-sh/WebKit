import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

let wat = `
(module
    (memory (export "memory") 1 1)
    (data (i32.const 0x0) "jinx is a good cat")
    (func (export "test") (param i32) (result i32)
        (local.get 0)
        (i32.load8_u)
        (return)
    )
)
`

async function test() {
    const instance = await instantiate(wat, {});
    const { memory, test } = instance.exports
    // Bun: a trap's message has no source text. Upstream expects the suffix " (evaluating 'test(65536)')".
    assert.throws(() => {test(65536)}, WebAssembly.RuntimeError, "Out of bounds memory access")
}

await assert.asyncTest(test())
