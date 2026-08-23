//@ requireOptions("--useDollarVM=1")

let ab;
try {
    ab = new ArrayBuffer(4 * 2 ** 30);
} catch (e) {
    quit();
}
Object.assign(Uint8Array.prototype, $vm.createBufferAccessors());

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}
function shouldThrow(f, expected, message) {
    let error = null;
    try {
        f();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof expected))
        throw new Error(message + ": expected a " + expected.name + " but got " + error);
}

const tailOffset = 4 * 2 ** 30 - 64;
const tail = new Uint8Array(ab, tailOffset, 64);
const wide = new Uint8Array(ab, 2 ** 31);
const raw = new DataView(ab);

function readAt(v, o) { return v.readInt32LE(o); }
function writeAt(v, x, o) { return v.writeInt32LE(x, o); }
noInline(readAt);
noInline(writeAt);

const iterations = testLoopCount * 30;
for (let i = 0; i < iterations; ++i) {
    shouldBe(writeAt(tail, i, 8), 12, "write into the ~4GB byteOffset view");
    shouldBe(readAt(tail, 8), i, "read the ~4GB byteOffset view back");
    shouldBe(writeAt(wide, ~i, wide.length - 4), wide.length, "write at the top of the 2GB byteOffset view");
    shouldBe(readAt(wide, wide.length - 4), ~i, "read at the top of the 2GB byteOffset view");
}
shouldBe(raw.getInt32(tailOffset + 8, true), iterations - 1, "the store landed at byteOffset + offset in the raw buffer");
shouldBe(raw.getInt32(2 ** 31 + wide.length - 4, true), ~(iterations - 1), "the store landed at the 2GB byteOffset");
shouldThrow(() => readAt(tail, 61), RangeError, "straddling the end of the small view");
shouldThrow(() => writeAt(wide, 0, wide.length - 3), RangeError, "straddling the end of the wide view");
shouldBe(numberOfDFGCompiles(readAt) <= 3, true, "the huge byteOffset does not cause recompiles");
