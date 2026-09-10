//@ skip if $addressBits <= 32
//@ runDefaultWasm("-m", "--useJIT=0")

import Builder from '../Builder.js'
import * as assert from '../assert.js'

// Without the JIT, a call between wasm and JS is marshalled by the C++
// operations in WasmOperations.cpp. They address the two frames by a byte
// offset from a frame pointer, and some of those offsets are negative: the
// callable function sits below the wasm-to-JS frame pointer, and the entry
// wrapper writes the callee's stack arguments below the JS-to-wasm frame
// pointer.

function makeInstance(params, ret, f) {
    let code = new Builder()
        .Type().End()
        .Import()
            .Function("imp", "f", { params, ret })
        .End()
        .Function().End()
        .Export()
            .Function("g")
        .End()
        .Code()
            .Function("g", { params, ret });
    for (let i = 0; i < params.length; ++i)
        code = code.GetLocal(i);
    const builder = code.Call(0).Return().End().End();
    const module = new WebAssembly.Module(builder.WebAssembly().get());
    return new WebAssembly.Instance(module, { imp: { f } });
}

// The smallest call that reads the callable function slot.
{
    let calls = 0;
    const instance = makeInstance([], "void", () => { ++calls; });
    for (let i = 0; i < 1000; ++i)
        instance.exports.g();
    assert.eq(calls, 1000);
}

// More arguments than there are argument registers, so both directions also
// marshal stack slots.
{
    const params = [];
    const args = [];
    for (let i = 0; i < 6; ++i) {
        params.push("i32", "i64", "f32", "f64");
        args.push(i, BigInt(i), i + 0.5, i + 0.25);
    }

    let seen = null;
    const instance = makeInstance(params, "f64", (...a) => { seen = a; return 1.5; });
    for (let i = 0; i < 1000; ++i)
        assert.eq(instance.exports.g(...args), 1.5);

    assert.eq(seen.length, args.length);
    for (let i = 0; i < args.length; ++i)
        assert.eq(seen[i], args[i]);
}
