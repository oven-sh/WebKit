//@ requireOptions("--forceBufferMemoryLimit=50331648")

// A transfer of a pinned ArrayBuffer copies it (see pinned-resizable-array-buffer-transfer.js). The copy of a resizable
// buffer is a resizable buffer, and that takes its pages from a budget of three times the memory of the machine.
// --forceBufferMemoryLimit makes the budget 48 MiB here.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function shouldThrow(callback, errorConstructor, message) {
    let error;
    try {
        callback();
    } catch (thrown) {
        error = thrown;
    }
    if (!(error instanceof errorConstructor))
        throw new Error(`${message}: expected a ${errorConstructor.name} but got ${error}`);
}

function shape(buffer) {
    return `${buffer.byteLength} of ${buffer.maxByteLength}, ${buffer.resizable ? "resizable" : "fixed"}, ${buffer.detached ? "detached" : "attached"}`;
}

function bytes(buffer) {
    return Array.from(new Uint8Array(buffer)).join(",");
}

function makeBuffer(length, options) {
    const buffer = new ArrayBuffer(length, options);
    const view = new Uint8Array(buffer);
    for (let i = 0; i < view.length; i++)
        view[i] = i + 1;
    return buffer;
}

const first16 = "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16";

// Made in a frame that is gone when it is freed, so that only the array refers to it. It is the room for 16 copies.
function makeRoom() {
    return [new ArrayBuffer(1024 * 1024, { maxByteLength: 1024 * 1024 })];
}

const source = makeBuffer(16, { maxByteLength: 1024 * 1024 });
$vm.pinArrayBuffer(source);
const room = makeRoom();
const held = [];
for (let size = 32 * 1024 * 1024; size >= 65536; size /= 2) {
    try {
        for (;;)
            held.push(new ArrayBuffer(size, { maxByteLength: size }));
    } catch { }
}

// With no room for the copy, transfer() throws and leaves the buffer as it is. transferToFixedLength() makes no such
// copy.
for (const newLength of [undefined, 8, 32]) {
    shouldThrow(() => source.transfer(newLength), RangeError, `transfer(${newLength}) of a pinned resizable buffer with no room for the copy`);
    shouldBe(shape(source), "16 of 1048576, resizable, attached", `the pinned source of a transfer(${newLength}) that failed`);
}
shouldBe(shape(source.transferToFixedLength()), "16 of 16, fixed, attached", "transferToFixedLength() with no room for a resizable copy");
shouldBe(shape(source.transferToFixedLength(32)), "32 of 32, fixed, attached", "transferToFixedLength(32) with no room for a resizable copy");
shouldBe(bytes(source), first16, "bytes of the pinned source with no room for a resizable copy");

// With room for 16 copies, the copies that nothing holds any more are collected to make the next one.
room[0] = null;
gc();
for (let i = 0; i < 64; i++)
    shouldBe(shape(source.transfer()), "16 of 1048576, resizable, attached", `transfer() number ${i} with room for 16 copies`);
