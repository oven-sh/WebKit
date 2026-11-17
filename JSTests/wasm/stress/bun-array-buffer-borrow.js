import * as assert from "../assert.js";

// grow() detaches a borrowed buffer. The memory goes on owning the bytes.
const memory = new WebAssembly.Memory({ initial: 1 });
const buffer = memory.buffer;
const borrow = $vm.borrowBytes(buffer);
memory.grow(0);
assert.eq(buffer.detached, true);
borrow("fill", 1);
borrow("end");
assert.eq(new Uint8Array(memory.buffer)[65535], 1);
