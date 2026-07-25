//@ requireOptions("--useDollarVM=1")

let big;
try {
  big = new Uint8Array(3 * 2 ** 30);
} catch (e) {
  quit();
}
Object.assign(Uint8Array.prototype, $vm.createBufferAccessors());

function shouldBe(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ": expected " + expected + " but got " + actual);
}
function shouldThrow(f, expected, message) {
  let error = null;
  try {
    f();
  } catch (e) {
    error = e;
  }
  if (!(error instanceof expected)) throw new Error(message + ": expected a " + expected.name + " but got " + error);
}

function readAt(b, o) {
  return b.readInt32LE(o);
}
function writeAt(b, v, o) {
  return b.writeInt32LE(v, o);
}
noInline(readAt);
noInline(writeAt);

const small = new Uint8Array(64);
const top = 2 ** 31 - 4;
for (let i = 0; i < 5e5; ++i) {
  shouldBe(writeAt(big, i, 100), 104, "write low");
  shouldBe(readAt(big, 100), i, "read low");
  shouldBe(writeAt(big, ~i, top), top + 4, "write at the int32 offset ceiling");
  shouldBe(readAt(big, top), ~i, "read at the int32 offset ceiling");
  shouldBe(writeAt(small, i, 60), 64, "the same site with a small receiver");
  shouldBe(readAt(small, 60), i, "read the small receiver back");
}
shouldBe(numberOfDFGCompiles(readAt) <= 3, true, "the large receiver does not cause recompiles");
shouldBe(numberOfDFGCompiles(writeAt) <= 3, true, "the large receiver does not cause recompiles");
shouldThrow(() => readAt(big, big.length - 3), RangeError, "straddling the end of a >2GB view");
shouldThrow(() => writeAt(big, 0, big.length), RangeError, "past the end of a >2GB view");
