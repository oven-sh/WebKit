import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

let wat = `
(module
    (func (export "test")
        (unreachable)
        (return)
    )
)
`

async function test() {
    const instance = await instantiate(wat, {});
    const { test } = instance.exports
    // Bun: a trap's message has no source text. Upstream expects the suffix " (evaluating 'test()')".
    assert.throws(() => {test()}, WebAssembly.RuntimeError, "Unreachable code should not be executed");
}

await assert.asyncTest(test())
