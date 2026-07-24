//@ requireOptions("--useDollarVM=1")

function shouldBe(actual, expected, message) {
  if (Number.isNaN(expected)) {
    if (!Number.isNaN(actual)) throw new Error(message + ": expected NaN but got " + actual);
    return;
  }
  if (actual !== expected) throw new Error(message + ": expected " + expected + " but got " + actual);
}

const accessors = $vm.createBufferAccessors();
class Buffer extends Uint8Array {}
Object.assign(Buffer.prototype, accessors);

const buf = new Buffer(64);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
for (let i = 0; i < buf.length; ++i) buf[i] = (i * 37 + 11) & 0xff;

let readers = [
  ["readInt8", o => dv.getInt8(o)],
  ["readUInt8", o => dv.getUint8(o)],
  ["readInt16LE", o => dv.getInt16(o, true)],
  ["readInt16BE", o => dv.getInt16(o, false)],
  ["readUInt16LE", o => dv.getUint16(o, true)],
  ["readUInt16BE", o => dv.getUint16(o, false)],
  ["readInt32LE", o => dv.getInt32(o, true)],
  ["readInt32BE", o => dv.getInt32(o, false)],
  ["readUInt32LE", o => dv.getUint32(o, true)],
  ["readUInt32BE", o => dv.getUint32(o, false)],
  ["readFloatLE", o => dv.getFloat32(o, true)],
  ["readFloatBE", o => dv.getFloat32(o, false)],
  ["readDoubleLE", o => dv.getFloat64(o, true)],
  ["readDoubleBE", o => dv.getFloat64(o, false)],
  ["readBigInt64LE", o => dv.getBigInt64(o, true)],
  ["readBigInt64BE", o => dv.getBigInt64(o, false)],
  ["readBigUInt64LE", o => dv.getBigUint64(o, true)],
  ["readBigUInt64BE", o => dv.getBigUint64(o, false)],
];
for (let [name, reference] of readers) {
  let read = new Function("b", "o", `return b.${name}(o);`);
  noInline(read);
  for (let i = 0; i < 1e4; ++i) {
    let o = i & 31;
    shouldBe(read(buf, o), reference(o), name + " @" + o);
  }
  let readDefault = new Function("b", `return b.${name}();`);
  noInline(readDefault);
  let accessor = accessors[name];
  let readPlain = function (b) {
    return accessor.call(b, 0);
  };
  noInline(readPlain);
  let plain = new Uint8Array(buf);
  for (let i = 0; i < 1e4; ++i) {
    shouldBe(readDefault(buf), reference(0), name + " default offset");
    shouldBe(readPlain(plain), reference(0), name + " on a plain Uint8Array");
  }
}

let writers = [
  ["writeInt8", o => dv.getInt8(o), i => (i & 0xff) - 128],
  ["writeUInt8", o => dv.getUint8(o), i => i & 0xff],
  ["writeInt16LE", o => dv.getInt16(o, true), i => (i & 0xffff) - 0x8000],
  ["writeInt16BE", o => dv.getInt16(o, false), i => (i & 0xffff) - 0x8000],
  ["writeUInt16LE", o => dv.getUint16(o, true), i => i & 0xffff],
  ["writeUInt16BE", o => dv.getUint16(o, false), i => i & 0xffff],
  ["writeInt32LE", o => dv.getInt32(o, true), i => (-i * 1000) | 0],
  ["writeInt32BE", o => dv.getInt32(o, false), i => (i * 1000) | 0],
  ["writeUInt32LE", o => dv.getUint32(o, true), i => 4294967295 - i],
  ["writeUInt32BE", o => dv.getUint32(o, false), i => 2147483648 + i],
  ["writeFloatLE", o => dv.getFloat32(o, true), i => Math.fround(i / 3)],
  ["writeFloatBE", o => dv.getFloat32(o, false), i => Math.fround(-i / 7)],
  ["writeDoubleLE", o => dv.getFloat64(o, true), i => i + 0.25],
  ["writeDoubleBE", o => dv.getFloat64(o, false), i => -i - 0.5],
];
for (let [name, reference, value] of writers) {
  let byteSize = name.match(/8/) ? 1 : name.match(/16/) ? 2 : name.match(/Float/) ? 4 : name.match(/32/) ? 4 : 8;
  let write = new Function("b", "v", "o", `return b.${name}(v, o);`);
  noInline(write);
  for (let i = 0; i < 1e4; ++i) {
    let o = i & 31;
    let v = value(i);
    shouldBe(write(buf, v, o), o + byteSize, name + " result @" + o);
    shouldBe(reference(o), v, name + " store @" + o);
  }
  let writeDefault = new Function("b", "v", `return b.${name}(v);`);
  noInline(writeDefault);
  for (let i = 0; i < 1e4; ++i) {
    shouldBe(writeDefault(buf, value(i)), byteSize, name + " default offset");
    shouldBe(reference(0), value(i), name + " default offset store");
  }
}

function writeInt32LEValue(b, v) {
  return b.writeInt32LE(v, 12);
}
noInline(writeInt32LEValue);
function writeUInt32LEValue(b, v) {
  return b.writeUInt32LE(v, 16);
}
noInline(writeUInt32LEValue);
for (let i = 0; i < 1e4; ++i) {
  shouldBe(writeInt32LEValue(buf, i + 0.75), 16, "writeInt32LE fractional result");
  shouldBe(dv.getInt32(12, true), i, "writeInt32LE fractional store");
  shouldBe(writeInt32LEValue(buf, NaN), 16, "writeInt32LE NaN result");
  shouldBe(dv.getInt32(12, true), 0, "writeInt32LE NaN store");
  shouldBe(writeUInt32LEValue(buf, 4294967295), 20, "writeUInt32LE max result");
  shouldBe(dv.getUint32(16, true), 4294967295, "writeUInt32LE max store");
  shouldBe(writeUInt32LEValue(buf, 1.5), 20, "writeUInt32LE fractional result");
  shouldBe(dv.getUint32(16, true), 1, "writeUInt32LE fractional store");
  shouldBe(
    writeInt32LEValue(buf, {
      valueOf() {
        return 7;
      },
    }),
    16,
    "writeInt32LE valueOf result",
  );
  shouldBe(dv.getInt32(12, true), 7, "writeInt32LE valueOf store");
}

{
  const impure = new Buffer(16);
  impure.fill(0xff);
  const dvImpure = new DataView(impure.buffer);
  const readFloat = new Function("b", "o", "return b.readFloatLE(o);");
  const readDouble = new Function("b", "o", "return b.readDoubleLE(o);");
  noInline(readFloat);
  noInline(readDouble);
  for (let i = 0; i < 1e4; ++i) {
    const f = readFloat(impure, 0);
    const d = readDouble(impure, 8);
    shouldBe(f, dvImpure.getFloat32(0, true), "impure NaN float matches the DataView reference");
    shouldBe(d, dvImpure.getFloat64(8, true), "impure NaN double matches the DataView reference");
    shouldBe(f + 1, NaN, "the boxed NaN stays usable in arithmetic");
    shouldBe(d * 2, NaN, "the boxed NaN stays usable in arithmetic");
  }
}
