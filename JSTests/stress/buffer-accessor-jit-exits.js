//@ requireOptions("--useDollarVM=1")

// The Buffer accessor nodes speculate: a Uint8Array receiver, an int32 in-bounds offset, an in-range
// value. Everything else must OSR-exit to the host function and behave exactly as it does in the
// interpreter -- in particular an out-of-bounds access must always throw, never return undefined,
// and no store may happen (or happen twice) around an exit.

function shouldThrow(f, expected, message) {
  let error = null;
  try {
    f();
  } catch (e) {
    error = e;
  }
  if (!error) throw new Error(message + ": expected a " + expected.name + " but got no throw");
  if (!(error instanceof expected)) throw new Error(message + ": expected a " + expected.name + " but got " + error);
}
function shouldBe(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ": expected " + expected + " but got " + actual);
}

const accessors = $vm.createBufferAccessors();
class Buffer extends Uint8Array {}
Object.assign(Buffer.prototype, accessors);

const buf = new Buffer(16);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

// Offsets that leave the fast path.
function readInt32LE(b, o) {
  return b.readInt32LE(o);
}
noInline(readInt32LE);
function readUInt8(b, o) {
  return b.readUInt8(o);
}
noInline(readUInt8);
for (let i = 0; i < 1e4; ++i) {
  dv.setInt32(12, i, true);
  shouldBe(readInt32LE(buf, 12), i, "last valid offset");
  shouldThrow(() => readInt32LE(buf, 13), RangeError, "one past the last valid offset");
  shouldThrow(() => readInt32LE(buf, 16), RangeError, "offset === length");
  shouldThrow(() => readInt32LE(buf, -1), RangeError, "negative offset");
  shouldThrow(() => readInt32LE(buf, 1.5), RangeError, "fractional offset");
  shouldThrow(() => readInt32LE(buf, NaN), RangeError, "NaN offset");
  shouldThrow(() => readInt32LE(buf, Infinity), RangeError, "Infinity offset");
  shouldThrow(() => readInt32LE(buf, "0"), RangeError, "string offset");
  shouldBe(readInt32LE(buf, 4.0), dv.getInt32(4, true), "integral double offset");
  shouldBe(readUInt8(buf, 15), buf[15], "last byte");
  shouldThrow(() => readUInt8(buf, 16), RangeError, "one-byte read one past the end");
}

// Values that leave the fast path: the narrow writers range-check.
function writeInt8(b, v, o) {
  return b.writeInt8(v, o);
}
noInline(writeInt8);
function writeUInt16BE(b, v, o) {
  return b.writeUInt16BE(v, o);
}
noInline(writeUInt16BE);
function writeUInt32LE(b, v, o) {
  return b.writeUInt32LE(v, o);
}
noInline(writeUInt32LE);
for (let i = 0; i < 1e4; ++i) {
  shouldBe(writeInt8(buf, 127, 3), 4, "writeInt8 max");
  shouldBe(dv.getInt8(3), 127, "writeInt8 max store");
  shouldBe(writeInt8(buf, -128, 3), 4, "writeInt8 min");
  shouldBe(dv.getInt8(3), -128, "writeInt8 min store");
  shouldThrow(() => writeInt8(buf, 128, 3), RangeError, "writeInt8 too big");
  shouldThrow(() => writeInt8(buf, -129, 3), RangeError, "writeInt8 too small");
  shouldBe(dv.getInt8(3), -128, "a throwing writeInt8 stores nothing");
  shouldThrow(() => writeUInt16BE(buf, -1, 2), RangeError, "writeUInt16BE negative");
  shouldThrow(() => writeUInt16BE(buf, 65536, 2), RangeError, "writeUInt16BE too big");
  shouldBe(writeUInt16BE(buf, 65535, 2), 4, "writeUInt16BE max");
  shouldBe(dv.getUint16(2, false), 65535, "writeUInt16BE max store");
  shouldThrow(() => writeUInt32LE(buf, -1, 4), RangeError, "writeUInt32LE negative");
  shouldThrow(() => writeUInt32LE(buf, 4294967296, 4), RangeError, "writeUInt32LE too big");
  shouldThrow(() => writeUInt32LE(buf, 4294967295, 14), RangeError, "writeUInt32LE out of bounds");
  shouldBe(writeUInt32LE(buf, 4294967295, 4), 8, "writeUInt32LE max");
  shouldBe(dv.getUint32(4, true), 4294967295, "writeUInt32LE max store");
}

// Receivers other than a Uint8Array (Buffer) exit the CheckArray and take the host path, which
// (unlike the JIT'd form) accepts any ArrayBufferView with byte-length semantics and rejects the rest.
{
  const floats = new Float64Array(4);
  const dataView = new DataView(new ArrayBuffer(8));
  const otherView = new Uint32Array(4);
  function readOnAnything(b, o) {
    return accessors.readInt32LE.call(b, o);
  }
  noInline(readOnAnything);
  for (let i = 0; i < 1e4; ++i) {
    floats[0] = i;
    shouldBe(readOnAnything(buf, 0), dv.getInt32(0, true), "Buffer receiver");
    shouldBe(readOnAnything(floats, 0), new DataView(floats.buffer).getInt32(0, true), "Float64Array receiver");
    shouldBe(readOnAnything(dataView, 4), 0, "DataView receiver");
    shouldBe(readOnAnything(otherView, 12), 0, "Uint32Array receiver (byte semantics)");
    shouldThrow(() => readOnAnything({}, 0), TypeError, "plain object receiver");
    shouldThrow(() => readOnAnything(null, 0), TypeError, "null receiver");
  }
}

// A detached receiver has length 0: always the host path, always a RangeError.
{
  const detached = new Buffer(16);
  function readDetached(b) {
    return b.readUInt16LE(0);
  }
  noInline(readDetached);
  for (let i = 0; i < 1e3; ++i) shouldBe(readDetached(detached), 0, "before detach");
  transferArrayBuffer(detached.buffer);
  for (let i = 0; i < 1e3; ++i) shouldThrow(() => readDetached(detached), RangeError, "after detach");
}

// A write's value coercion happens exactly once even when the offset then fails (the JIT exits
// before any effect; the host coerces first and validates the offset second).
{
  let calls = 0;
  const value = {
    valueOf() {
      calls++;
      return 5;
    },
  };
  function writeWithBadOffset(b, o) {
    return b.writeInt32LE(value, o);
  }
  noInline(writeWithBadOffset);
  for (let i = 0; i < 1e3; ++i) {
    shouldBe(writeWithBadOffset(buf, 0), 4, "good offset");
    shouldThrow(() => writeWithBadOffset(buf, 100), RangeError, "bad offset");
  }
  shouldBe(calls, 2000, "valueOf calls");
}
