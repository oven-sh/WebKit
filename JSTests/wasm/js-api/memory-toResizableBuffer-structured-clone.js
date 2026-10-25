//@ requireOptions("--useWasmMemoryToBufferAPIs=true")

import Builder from "../Builder.js";
import * as assert from "../assert.js";

const pageSize = 64 * 1024;

// A structured clone of a shared memory's growable buffer shares the contents of the memory.
// It follows the rule of the buffer that the memory returned: the byte length is a page multiple.

function instantiate(memory, maximum) {
    const builder = (new Builder())
        .Type().End()
        .Import().Memory("imp", "memory", { initial: 0, maximum, shared: true }).End()
        .Function().End()
        .Export().Function("size").Function("grow").End()
        .Code()
            .Function("size", { params: [], ret: "i32" }).CurrentMemory(0).Return().End()
            .Function("grow", { params: ["i32"], ret: "i32" }).GetLocal(0).GrowMemory(0).Return().End()
        .End();
    return new WebAssembly.Instance(new WebAssembly.Module(builder.WebAssembly().get()), { imp: { memory } }).exports;
}

function assertGrowFails(buffer, newByteLength) {
    assert.throws(() => buffer.grow(newByteLength), RangeError, `grow failed with new byte length ${newByteLength}`);
}

{
    const memory = new WebAssembly.Memory({ initial: 1, maximum: 5, shared: true });
    const wasm = instantiate(memory, 5);
    const buffer = memory.toResizableBuffer();
    const clone = structuredClone(buffer);
    const cloneOfClone = structuredClone(clone);
    const view = structuredClone(new Uint8Array(buffer));
    const holders = [buffer, clone, cloneOfClone, view.buffer];

    function assertPageCount(pageCount) {
        for (const holder of holders)
            assert.eq(holder.byteLength, pageCount * pageSize);
        assert.eq(view.length, pageCount * pageSize);
        assert.eq(memory.buffer, buffer);
        assert.eq(wasm.size(), pageCount);
    }

    for (const alias of [clone, cloneOfClone, view.buffer]) {
        assert.truthy(alias instanceof SharedArrayBuffer);
        assert.truthy(alias !== buffer);
        assert.truthy(alias.growable);
        assert.eq(alias.maxByteLength, 5 * pageSize);
    }
    assertPageCount(1);

    // A byte length that is not a page multiple fails through every buffer, and the memory stays as it was.
    for (const holder of holders) {
        for (const newByteLength of [pageSize + 1, 2 * pageSize - 1, 4 * pageSize + 1])
            assertGrowFails(holder, newByteLength);
        assertPageCount(1);
    }

    // A page multiple grows the memory for every holder.
    clone.grow(2 * pageSize);
    assertPageCount(2);
    clone.grow(2 * pageSize);
    assertPageCount(2);

    // The memory still grows through Memory.prototype.grow and through the memory.grow instruction.
    assert.eq(memory.grow(1), 2);
    assertPageCount(3);
    assert.eq(wasm.grow(1), 3);
    assertPageCount(4);

    assertGrowFails(clone, 3 * pageSize);
    assertGrowFails(clone, 6 * pageSize);
    cloneOfClone.grow(5 * pageSize);
    assertPageCount(5);
    assert.eq(wasm.grow(1), -1);
}

{
    const memory = new WebAssembly.Memory({ initial: 0, maximum: 2, shared: true });
    const clone = structuredClone(memory.toResizableBuffer());
    assert.eq(clone.byteLength, 0);
    assertGrowFails(clone, 1);
    assert.eq(memory.buffer.byteLength, 0);
    clone.grow(pageSize);
    assert.eq(memory.buffer.byteLength, pageSize);
}

// A memory with a maximum of zero has nothing to grow.
{
    const memory = new WebAssembly.Memory({ initial: 0, maximum: 0, shared: true });
    const clone = structuredClone(memory.toResizableBuffer());
    assert.truthy(clone.growable);
    clone.grow(0);
    assertGrowFails(clone, 1);
    assert.eq(clone.byteLength, 0);
}

// The clone of a fixed-length buffer is a fixed-length buffer.
{
    const memory = new WebAssembly.Memory({ initial: 1, maximum: 2, shared: true });
    const clone = structuredClone(memory.buffer);
    assert.falsy(clone.growable);
    assert.throws(() => clone.grow(2 * pageSize), TypeError, "SharedArrayBuffer is not growable");
    assert.eq(clone.byteLength, pageSize);
}

// A growable SharedArrayBuffer that is not a memory has no page rule, and neither has its clone.
{
    const buffer = new SharedArrayBuffer(8, { maxByteLength: 2 * pageSize });
    const clone = structuredClone(buffer);
    clone.grow(9);
    assert.eq(buffer.byteLength, 9);
    clone.grow(pageSize + 1);
    assert.eq(buffer.byteLength, pageSize + 1);
}
