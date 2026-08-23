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

const buf = new Buffer(64);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

function writeBigInt64LE(b, v, o) {
  return b.writeBigInt64LE(v, o);
}
noInline(writeBigInt64LE);
function writeBigInt64BE(b, v, o) {
  return b.writeBigInt64BE(v, o);
}
noInline(writeBigInt64BE);
function writeBigUInt64LE(b, v, o) {
  return b.writeBigUInt64LE(v, o);
}
noInline(writeBigUInt64LE);
function writeBigUInt64BE(b, v, o) {
  return b.writeBigUInt64BE(v, o);
}
noInline(writeBigUInt64BE);

const values = [0n, 1n, -1n, 42n, -42n, 2n ** 31n, -(2n ** 31n), 2n ** 32n + 7n, 2n ** 63n - 1n, -(2n ** 63n)];
for (let i = 0; i < testLoopCount * 2; ++i) {
  const o = (i & 7) * 8;
  const v = values[i % values.length];
  shouldBe(writeBigInt64LE(buf, v, o), o + 8, "writeBigInt64LE result");
  shouldBe(dv.getBigInt64(o, true), v, "writeBigInt64LE store");
  shouldBe(writeBigInt64BE(buf, v, o), o + 8, "writeBigInt64BE result");
  shouldBe(dv.getBigInt64(o, false), v, "writeBigInt64BE store");
  if (v >= 0n) {
    shouldBe(writeBigUInt64LE(buf, v, o), o + 8, "writeBigUInt64LE result");
    shouldBe(dv.getBigUint64(o, true), v, "writeBigUInt64LE store");
    shouldBe(writeBigUInt64BE(buf, v, o), o + 8, "writeBigUInt64BE result");
    shouldBe(dv.getBigUint64(o, false), v, "writeBigUInt64BE store");
  }
}

for (let i = 0; i < testLoopCount * 2; ++i) {
  shouldBe(writeBigUInt64LE(buf, 2n ** 64n - 1n, 0), 8, "unsigned max");
  shouldBe(dv.getBigUint64(0, true), 2n ** 64n - 1n, "unsigned max store");
  shouldThrow(() => writeBigUInt64LE(buf, -1n, 0), RangeError, "unsigned negative");
  shouldThrow(() => writeBigUInt64LE(buf, 2n ** 64n, 0), RangeError, "unsigned too big");
  shouldThrow(() => writeBigInt64LE(buf, 2n ** 63n, 0), RangeError, "signed too big");
  shouldThrow(() => writeBigInt64LE(buf, -(2n ** 63n) - 1n, 0), RangeError, "signed too small");
  shouldThrow(() => writeBigInt64LE(buf, 2n ** 100n, 0), RangeError, "way too big");
  shouldThrow(() => writeBigInt64LE(buf, 5, 0), TypeError, "a number is not a BigInt");
  shouldThrow(() => writeBigInt64LE(buf, 0n, 57), RangeError, "out of bounds");
  shouldBe(dv.getBigInt64(0, true), -1n, "the failed writes stored nothing");
}
