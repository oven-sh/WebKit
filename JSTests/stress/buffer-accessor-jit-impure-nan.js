//@ requireOptions("--useDollarVM=1")

// The float readers load raw bytes, so they can produce any NaN bit pattern, including the ones
// JSC uses to box cells and int32s. The loaded double must be treated as possibly-impure: whenever
// it is boxed (returned, stored to a property or an array, compared as a JSValue) it has to come out
// as the one canonical NaN, in every tier.

function shouldBe(actual, expected, message) {
  if (!Object.is(actual, expected)) throw new Error(message + ": expected " + expected + " but got " + actual);
}

const accessors = $vm.createBufferAccessors();
class Buffer extends Uint8Array {}
Object.assign(Buffer.prototype, accessors);

const buf = new Buffer(64);
const dv = new DataView(buf.buffer);
const someObject = { marker: 42 };

// Bit patterns: a pure NaN, an "impure" NaN in the range JSC uses for boxed int32s / cells if it
// were reinterpreted as a JSValue, negative NaNs, and signalling NaNs, as both float64 and float32.
const doublePatterns = [
  [0x7ff80000, 0x00000000],
  [0xfffe0000, 0x00001234], // would be a boxed int32 if not purified
  [0xffff0000, 0x00000000],
  [0xfffc0000, 0x12345678],
  [0x7ff00000, 0x00000001], // signalling
  [0xfff00000, 0x00000001],
  [0x7fffffff, 0xffffffff],
  [0xffffffff, 0xffffffff],
  [0x0001ffff, 0xfffffff0], // not a NaN: a tiny denormal-ish double, must round-trip exactly
];
const floatPatterns = [0x7fc00000, 0x7f800001, 0xffc00000, 0xffffffff, 0x7fffffff, 0xff800001];

function readDoubleLE(b, o) { return b.readDoubleLE(o); }
function readDoubleBE(b, o) { return b.readDoubleBE(o); }
function readFloatLE(b, o) { return b.readFloatLE(o); }
function readFloatBE(b, o) { return b.readFloatBE(o); }
noInline(readDoubleLE); noInline(readDoubleBE); noInline(readFloatLE); noInline(readFloatBE);

// Boxing through different paths: return value, property store, array store, arithmetic, typeof.
function storeToObject(b, o, target) { target.value = b.readDoubleLE(o); return target; }
noInline(storeToObject);
function storeToArray(b, o, target) { target[0] = b.readDoubleBE(o); return target; }
noInline(storeToArray);
function addZero(b, o) { return b.readFloatLE(o) + 0; }
noInline(addZero);
function typeOf(b, o) { return typeof b.readFloatBE(o); }
noInline(typeOf);
function isCellLike(b, o) { let v = b.readDoubleLE(o); return v === someObject || (typeof v)[0] !== "n"; }
noInline(isCellLike);

for (let i = 0; i < testLoopCount; ++i) {
  const [hi, lo] = doublePatterns[i % doublePatterns.length];
  dv.setUint32(0, hi, false); dv.setUint32(4, lo, false);      // big-endian layout at 0
  dv.setUint32(8, lo, true); dv.setUint32(12, hi, true);       // little-endian layout at 8
  const expected = dv.getFloat64(0, false);
  shouldBe(readDoubleBE(buf, 0), expected, "readDoubleBE pattern " + i);
  shouldBe(readDoubleLE(buf, 8), expected, "readDoubleLE pattern " + i);

  const boxed = storeToObject(buf, 8, {}).value;
  shouldBe(boxed, expected, "property store");
  shouldBe(typeof boxed, "number", "property store type");
  const arr = storeToArray(buf, 0, [1.5]);
  shouldBe(arr[0], expected, "array store");
  shouldBe(typeof arr[0], "number", "array store type");
  const arr2 = storeToArray(buf, 0, [someObject]);
  shouldBe(arr2[0], expected, "contiguous array store");
  shouldBe(isCellLike(buf, 8), false, "never a cell");

  const f = floatPatterns[i % floatPatterns.length];
  dv.setUint32(16, f, true); dv.setUint32(20, f, false);
  const expectedFloat = dv.getFloat32(16, true);
  shouldBe(readFloatLE(buf, 16), expectedFloat, "readFloatLE pattern " + i);
  shouldBe(readFloatBE(buf, 20), expectedFloat, "readFloatBE pattern " + i);
  shouldBe(addZero(buf, 16), expectedFloat + 0, "float arithmetic");
  shouldBe(typeOf(buf, 20), "number", "float typeof");
}
