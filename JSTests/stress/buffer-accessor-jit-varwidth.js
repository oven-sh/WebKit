//@ requireOptions("--useDollarVM=1")

// read(U)Int{LE,BE}(offset, byteLength) / write(U)Int{LE,BE}(value, offset, byteLength): a constant
// byteLength of 1, 2 or 4 becomes the fixed-width JIT node; every other width (and a non-constant
// one) stays on the host function. All shapes must agree with the reference after tier-up.

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

class Buffer extends Uint8Array {}
Object.assign(Buffer.prototype, $vm.createBufferAccessors());

const buf = new Buffer(64);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
for (let i = 0; i < buf.length; ++i) buf[i] = (i * 71 + 5) & 0xff;

function readIntLE(b, o, l) {
  return b.readIntLE(o, l);
}
function readIntBE(b, o, l) {
  return b.readIntBE(o, l);
}
function readUIntLE(b, o, l) {
  return b.readUIntLE(o, l);
}
function readUIntBE(b, o, l) {
  return b.readUIntBE(o, l);
}
function writeIntLE(b, v, o, l) {
  return b.writeIntLE(v, o, l);
}
function writeUIntBE(b, v, o, l) {
  return b.writeUIntBE(v, o, l);
}
// Constant widths: these get the JIT node (1, 2, 4) or must stay correct without one (3).
function readInt32ConstLE(b, o) {
  return b.readIntLE(o, 4);
}
function readUInt16ConstBE(b, o) {
  return b.readUIntBE(o, 2);
}
function readUInt24ConstLE(b, o) {
  return b.readUIntLE(o, 3);
}
function writeInt8Const(b, v, o) {
  return b.writeIntLE(v, o, 1);
}
for (const f of [
  readIntLE,
  readIntBE,
  readUIntLE,
  readUIntBE,
  writeIntLE,
  writeUIntBE,
  readInt32ConstLE,
  readUInt16ConstBE,
  readUInt24ConstLE,
  writeInt8Const,
])
  noInline(f);

const uint = (o, l, le) => {
  let value = 0;
  for (let i = 0; i < l; ++i) value = le ? value + buf[o + i] * 2 ** (8 * i) : value * 256 + buf[o + i];
  return value;
};
const sint = (o, l, le) => {
  const value = uint(o, l, le);
  return value >= 2 ** (8 * l - 1) ? value - 2 ** (8 * l) : value;
};

for (let i = 0; i < 1e4; ++i) {
  const o = i & 15;
  for (const l of [1, 2, 3, 4, 5, 6]) {
    shouldBe(readIntLE(buf, o, l), sint(o, l, true), "readIntLE " + l);
    shouldBe(readIntBE(buf, o, l), sint(o, l, false), "readIntBE " + l);
    shouldBe(readUIntLE(buf, o, l), uint(o, l, true), "readUIntLE " + l);
    shouldBe(readUIntBE(buf, o, l), uint(o, l, false), "readUIntBE " + l);
  }
  shouldBe(readInt32ConstLE(buf, o), dv.getInt32(o, true), "readIntLE const 4");
  shouldBe(readUInt16ConstBE(buf, o), dv.getUint16(o, false), "readUIntBE const 2");
  shouldBe(readUInt24ConstLE(buf, o), uint(o, 3, true), "readUIntLE const 3");
}

const scratch = new Buffer(64);
const scratchDV = new DataView(scratch.buffer);
for (let i = 0; i < 1e4; ++i) {
  const o = i & 15;
  shouldBe(writeIntLE(scratch, -(i & 0x7fff), o, 4), o + 4, "writeIntLE 4 result");
  shouldBe(scratchDV.getInt32(o, true), -(i & 0x7fff), "writeIntLE 4 store");
  shouldBe(writeUIntBE(scratch, i & 0xffffff, o, 3), o + 3, "writeUIntBE 3 result");
  shouldBe(scratch[o] * 65536 + scratch[o + 1] * 256 + scratch[o + 2], i & 0xffffff, "writeUIntBE 3 store");
  shouldBe(writeInt8Const(scratch, (i & 0xff) - 128, o), o + 1, "writeIntLE const 1 result");
  shouldBe(scratchDV.getInt8(o), (i & 0xff) - 128, "writeIntLE const 1 store");
}

// The exits: non-constant widths that turn out invalid, a missing/undefined offset (no default here),
// out-of-range values and offsets -- all keep throwing after tier-up.
for (let i = 0; i < 3e3; ++i) {
  shouldThrow(() => readIntLE(buf, 0, 7), RangeError, "byteLength 7");
  shouldThrow(() => readIntLE(buf, 0, 0), RangeError, "byteLength 0");
  shouldThrow(() => readInt32ConstLE(buf, undefined), TypeError, "undefined offset");
  shouldThrow(() => readInt32ConstLE(buf, 61), RangeError, "out of bounds");
  shouldThrow(() => writeInt8Const(scratch, 128, 0), RangeError, "value out of range");
  shouldThrow(() => writeUIntBE(scratch, 2 ** 24, 0, 3), RangeError, "3-byte value out of range");
  // The $vm reference rejects NaN (Node itself stores 0): what matters here is that the JIT and the
  // host function agree, and that neither reaches an out-of-range float-to-int conversion.
  shouldThrow(() => writeIntLE(scratch, NaN, 0, 4), RangeError, "NaN value");
  shouldThrow(() => writeIntLE(scratch, Infinity, 0, 4), RangeError, "Infinity value");
  shouldThrow(() => writeIntLE(scratch, -Infinity, 0, 4), RangeError, "-Infinity value");
  shouldThrow(() => writeInt8Const(scratch, NaN, 0), RangeError, "NaN value, constant width");
}
