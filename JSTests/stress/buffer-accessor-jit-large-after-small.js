//@ requireOptions("--useDollarVM=1")

// Call sites compiled while only small receivers were seen use an int32 length. A receiver longer than
// INT32_MAX that shows up afterwards -- a different view, or the same resizable view grown in place --
// must be handled by an exit and a recompile that takes the length as an Int52, never by a truncated
// length: every access below has to land on the right byte or throw, and the sites must settle.

let big, resizable;
try {
  big = new Uint8Array(3 * 2 ** 30);
  resizable = new ArrayBuffer(64, { maxByteLength: 3 * 2 ** 30 });
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

function readAt(b, o) { return b.readInt32LE(o); }
function writeAt(b, v, o) { return b.writeInt32LE(v, o); }
function read8(b, o) { return b.readUInt8(o); }
function write8(b, v, o) { return b.writeUInt8(v, o); }
function readF(b, o) { return b.readDoubleBE(o); }
function writeF(b, v, o) { return b.writeDoubleBE(v, o); }
function sum(b, from, count) { let s = 0; for (let i = from, n = from + count; i < n; i++) s += b.readUInt8(i); return s; }
for (const f of [readAt, writeAt, read8, write8, readF, writeF, sum]) noInline(f);

// 1. Warm every site on a 64-byte view only.
const small = new Uint8Array(64);
for (let i = 0; i < testLoopCount * 20; ++i) {
  writeAt(small, i, 8); shouldBe(readAt(small, 8), i, "small int32");
  write8(small, i & 255, 3); shouldBe(read8(small, 3), i & 255, "small uint8");
  writeF(small, i + 0.5, 16); shouldBe(readF(small, 16), i + 0.5, "small double");
  sum(small, 0, 64);
}

// 2. Now a 3GB view at offsets on both sides of 2^31.
const top = 2 ** 31 - 8;
const offsets = [0, 60, top - 64, top, top + 4];
for (let i = 0; i < testLoopCount * 20; ++i) {
  const o = offsets[i % offsets.length];
  if (o + 4 <= 2 ** 31 - 1) {
    shouldBe(writeAt(big, i | 0, o), o + 4, "big write int32 @" + o);
    shouldBe(readAt(big, o), i | 0, "big read int32 @" + o);
    shouldBe(big[o] | (big[o + 1] << 8) | (big[o + 2] << 16) | (big[o + 3] << 24), i | 0, "bytes landed @" + o);
  }
  if (o <= 2 ** 31 - 1) {
    shouldBe(write8(big, i & 255, o), o + 1, "big write uint8 @" + o);
    shouldBe(read8(big, o), i & 255, "big read uint8 @" + o);
    shouldBe(big[o], i & 255, "byte landed @" + o);
  }
  if (o + 8 <= 2 ** 31 - 1) {
    writeF(big, i + 0.25, o); shouldBe(readF(big, o), i + 0.25, "big double @" + o);
  }
  // and the small one keeps working at the same sites
  writeAt(small, ~i, 8); shouldBe(readAt(small, 8), ~i, "small again");
}
shouldBe(sum(big, top - 256, 256) >= 0, true, "loop over the top of the int32 range");
shouldThrow(() => readAt(big, -1), RangeError, "negative offset on the big view");
big[2 ** 31] = 0x5a;
shouldBe(read8(big, 2 ** 31), 0x5a, "offset 2^31 is not an int32; the host path handles it");
shouldBe(read8(big, 2 ** 31 - 1), big[2 ** 31 - 1], "last int32 offset");
shouldThrow(() => readAt(big, big.length - 3), RangeError, "straddling the end of the big view");

// 3. A resizable view: compiled while 64 bytes long, then grown past INT32_MAX in place.
const growing = new Uint8Array(resizable);
function readG(b, o) { return b.readUInt16LE(o); }
function writeG(b, v, o) { return b.writeUInt16LE(v, o); }
noInline(readG); noInline(writeG);
for (let i = 0; i < testLoopCount * 20; ++i) { writeG(growing, i & 0xffff, 62); shouldBe(readG(growing, 62), i & 0xffff, "growing small"); }
shouldThrow(() => readG(growing, 63), RangeError, "past the small end");
resizable.resize(3 * 2 ** 30);
for (let i = 0; i < testLoopCount * 20; ++i) {
  const o = offsets[i % offsets.length];
  if (o + 2 > 2 ** 31 - 1) continue;
  shouldBe(writeG(growing, i & 0xffff, o), o + 2, "grown write @" + o);
  shouldBe(readG(growing, o), i & 0xffff, "grown read @" + o);
  shouldBe(growing[o] | (growing[o + 1] << 8), i & 0xffff, "grown bytes @" + o);
}
resizable.resize(32);
shouldThrow(() => readG(growing, 31), RangeError, "shrunk again");
shouldBe(readG(growing, 30) >= 0, true, "shrunk, in bounds");

for (const f of [readAt, writeAt, read8, write8, readF, writeF, sum, readG, writeG])
  shouldBe(numberOfDFGCompiles(f) <= 4 || numberOfDFGCompiles(f) == 1e6, true, f.name + " settled (" + numberOfDFGCompiles(f) + " compiles)");
