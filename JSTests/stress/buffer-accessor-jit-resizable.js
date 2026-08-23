//@ requireOptions("--useDollarVM=1")

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

const accessors = $vm.createBufferAccessors();
class Buffer extends Uint8Array {}
Object.assign(Buffer.prototype, accessors);

function readUInt16LE(b, o) {
  return b.readUInt16LE(o);
}
noInline(readUInt16LE);
function writeUInt16LE(b, v, o) {
  return b.writeUInt16LE(v, o);
}
noInline(writeUInt16LE);

{
  const fixed = new Buffer(64);
  for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(writeUInt16LE(fixed, i & 0xffff, i & 62), (i & 62) + 2, "fixed write");
    shouldBe(readUInt16LE(fixed, i & 62), i & 0xffff, "fixed read");
  }
}

{
  const rab = new ArrayBuffer(16, { maxByteLength: 64 });
  const tracking = new Buffer(rab);
  shouldBe(tracking.length, 16, "tracking length");
  for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(writeUInt16LE(tracking, i & 0xffff, 14), 16, "tracking write at the end");
    shouldBe(readUInt16LE(tracking, 14), i & 0xffff, "tracking read at the end");
    shouldThrow(() => readUInt16LE(tracking, 15), RangeError, "tracking read straddling the end");
    shouldThrow(() => readUInt16LE(tracking, 16), RangeError, "tracking read past the end");
  }
  rab.resize(64);
  shouldBe(tracking.length, 64, "grown tracking length");
  for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(writeUInt16LE(tracking, i & 0xffff, 62), 64, "write near the grown end");
    shouldBe(readUInt16LE(tracking, 62), i & 0xffff, "read near the grown end");
  }
  rab.resize(8);
  shouldBe(tracking.length, 8, "shrunk tracking length");
  for (let i = 0; i < testLoopCount / 10; ++i) {
    shouldBe(readUInt16LE(tracking, 6), 0, "read near the shrunk end (never written)");
    shouldThrow(() => readUInt16LE(tracking, 7), RangeError, "read straddling the shrunk end");
    shouldThrow(() => writeUInt16LE(tracking, 0, 62), RangeError, "write past the shrunk end");
  }
}

{
  const rab = new ArrayBuffer(32, { maxByteLength: 64 });
  const fixed = new Buffer(rab, 8, 16);
  shouldBe(fixed.length, 16, "fixed-length view length");
  for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(writeUInt16LE(fixed, i & 0xffff, 14), 16, "fixed-length view write");
    shouldBe(readUInt16LE(fixed, 14), i & 0xffff, "fixed-length view read");
  }
  rab.resize(16);
  for (let i = 0; i < testLoopCount / 10; ++i) {
    shouldThrow(() => readUInt16LE(fixed, 0), RangeError, "out-of-bounds view read");
    shouldThrow(() => writeUInt16LE(fixed, 0, 0), RangeError, "out-of-bounds view write");
  }
}

{
  const gsab = new SharedArrayBuffer(16, { maxByteLength: 64 });
  const shared = new Buffer(gsab);
  for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(writeUInt16LE(shared, i & 0xffff, 14), 16, "shared write at the end");
    shouldBe(readUInt16LE(shared, 14), i & 0xffff, "shared read at the end");
    shouldThrow(() => readUInt16LE(shared, 15), RangeError, "shared read past the end");
  }
  gsab.grow(64);
  for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(writeUInt16LE(shared, i & 0xffff, 62), 64, "write near the grown shared end");
    shouldBe(readUInt16LE(shared, 62), i & 0xffff, "read near the grown shared end");
  }
}
