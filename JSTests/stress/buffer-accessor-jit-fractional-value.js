//@ requireOptions("--useDollarVM=1")

function shouldBe(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ": expected " + expected + " but got " + actual);
}

const accessors = $vm.createBufferAccessors();
class Buffer extends Uint8Array {}
Object.assign(Buffer.prototype, accessors);

const buf = new Buffer(16);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

// A fractional double is a valid value for the integer writers (it truncates). The unsigned
// 32-bit writers take the value as an Int52, and an Int52Rep of a non-integral DoubleRep exits with
// Int52Overflow rather than BadType, and a value the DFG has proven to be a non-integral double exits
// with Uncountable; those exits have to stop the call site from being inlined again,
// or the function recompiles forever.
function writeUInt32LE(b, v, o) {
  return b.writeUInt32LE(v * 0.5, o);
}
noInline(writeUInt32LE);
function writeUIntLE4(b, v, o) {
  return b.writeUIntLE(v * 0.5, o, 4);
}
noInline(writeUIntLE4);
function writeInt8(b, v, o) {
  return b.writeInt8(v * 0.5, o);
}
noInline(writeInt8);

for (let i = 0; i < testLoopCount * 100; ++i) {
  let v = (i & 127) | 1;
  shouldBe(writeUInt32LE(buf, v, 0), 4, "writeUInt32LE return");
  shouldBe(dv.getUint32(0, true), (v * 0.5) >>> 0, "writeUInt32LE stored the truncated value");
  shouldBe(writeUIntLE4(buf, v, 4), 8, "writeUIntLE return");
  shouldBe(dv.getUint32(4, true), (v * 0.5) >>> 0, "writeUIntLE stored the truncated value");
  shouldBe(writeInt8(buf, v, 8), 9, "writeInt8 return");
  shouldBe(dv.getInt8(8), (v * 0.5) | 0, "writeInt8 stored the truncated value");
}
shouldBe(numberOfDFGCompiles(writeUInt32LE) <= 3, true, "a fractional value does not cause writeUInt32LE recompiles");
shouldBe(numberOfDFGCompiles(writeUIntLE4) <= 3, true, "a fractional value does not cause writeUIntLE recompiles");
shouldBe(numberOfDFGCompiles(writeInt8) <= 3, true, "a fractional value does not cause writeInt8 recompiles");
