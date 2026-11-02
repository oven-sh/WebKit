//@ requireOptions("--useDollarVM=1")
// An embedder can make an ArrayBuffer over memory that something outside JavaScript also writes
// (ArrayBufferExternalWriters::Yes). It says so through canBeWrittenConcurrently(), and the flag goes
// where the bytes go: a transfer keeps it, a copy of the bytes does not have it.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected} but got ${actual}`);
}
const written = value => $vm.canBeWrittenConcurrently(value);

const buffer = $vm.createArrayBufferWithExternalWriters(16);
shouldBe(Object.getPrototypeOf(buffer), ArrayBuffer.prototype, "an ordinary ArrayBuffer to JavaScript");
shouldBe(buffer.byteLength, 16, "byteLength");
shouldBe(written(buffer), true, "the buffer");
for (const view of [new Uint8Array(buffer), new Uint8Array(buffer, 4, 4), new Float64Array(buffer), new DataView(buffer, 1), new Uint8Array(buffer).subarray(2)])
    shouldBe(written(view), true, `a ${view.constructor.name} of it`);

// A copy of the bytes is memory that only JavaScript writes.
shouldBe(written(buffer.slice(0)), false, "ArrayBuffer#slice()");
shouldBe(written(new Uint8Array(buffer).slice()), false, "TypedArray#slice()");
shouldBe(written(new Uint8Array(new Uint8Array(buffer))), false, "a typed array made from a view");

// A transfer moves the bytes, and the flag with them.
const moved = buffer.transfer();
shouldBe(buffer.detached, true, "transfer() detaches");
shouldBe(written(buffer), false, "the detached buffer");
shouldBe(written(moved), true, "transfer()");
const fixed = moved.transferToFixedLength();
shouldBe(written(moved), false, "the buffer that transferToFixedLength() detached");
shouldBe(written(fixed), true, "transferToFixedLength()");
shouldBe(written(new Uint8Array(fixed)), true, "a view of the buffer it moved to");
// A transfer to another length is a copy.
shouldBe(written(fixed.transfer(8)), false, "transfer(8)");

// Memory that the engine allocates has no other writer. A SharedArrayBuffer has.
shouldBe(written(new ArrayBuffer(16)), false, "new ArrayBuffer");
shouldBe(written(new ArrayBuffer(16, { maxByteLength: 32 })), false, "a resizable ArrayBuffer");
shouldBe(written(new Uint8Array(16)), false, "a typed array with no ArrayBuffer");
shouldBe(written(new Uint8Array(2000)), false, "a large typed array with no ArrayBuffer");
shouldBe(written(new Uint8Array(new ArrayBuffer(16))), false, "a view of an ArrayBuffer");
shouldBe(written(new SharedArrayBuffer(16)), true, "a SharedArrayBuffer");
shouldBe(written(new Uint8Array(new SharedArrayBuffer(16))), true, "a view of a SharedArrayBuffer");
shouldBe(written({}), undefined, "not a buffer");
