//@ requireOptions("--useDollarVM=1")

const read = $vm.ffiReadObject();
function check(a, e, m) { if (!Object.is(a, e)) throw new Error(m + ": expected " + e + " got " + a); }
// Build a 32-byte buffer with known bytes and get its address via an FFI identity function.
const buf = new Uint8Array(32);
const dv = new DataView(buf.buffer);
dv.setUint8(0, 0xFF); dv.setInt8(1, -5); dv.setUint16(2, 0xBEEF, true); dv.setInt16(4, -12345, true);
dv.setUint32(8, 0xFFFFFFF0, true); dv.setInt32(12, -123456789, true);
dv.setFloat32(16, 1.5, true); dv.setFloat64(20, Math.PI, true);
dv.setBigInt64(0, dv.getBigInt64(0, true), true); // no-op, keeps layout
const idPtr = $vm.ffiFunction({ args: ["ptr"], returns: "ptr" }, $vm.ffiFixture("ffi_ptr_identity"), "id");
const addr = idPtr(buf); // number (address)
check(typeof addr, "number", "address is a number");
// Reference values via DataView.
const cases = [
  ["u8", 0, dv.getUint8(0)], ["i8", 1, dv.getInt8(1)],
  ["u16", 2, dv.getUint16(2, true)], ["i16", 4, dv.getInt16(4, true)],
  ["u32", 8, dv.getUint32(8, true)], ["i32", 12, dv.getInt32(12, true)],
  ["f32", 16, dv.getFloat32(16, true)], ["f64", 20, dv.getFloat64(20, true)],
  // unaligned reads (offset 1) must work
  ["u32", 1, dv.getUint32(1, true)], ["f64", 1, dv.getFloat64(1, true)], ["i16", 3, dv.getInt16(3, true)],
];
for (const [t, off, expected] of cases) check(read[t](addr, off), expected, "cold read." + t + "@" + off);
// i64/u64 (BigInt, host path only)
check(read.i64(addr, 0), dv.getBigInt64(0, true), "read.i64");
check(read.u64(addr, 8), dv.getBigUint64(8, true), "read.u64");
// ptr / intptr: 8-byte reads surfaced as doubles
// ptr/intptr over a slot holding a REAL user-space pointer (< 2^53) come back as numbers.
// (separate buffer so we do not clobber the f64 at offset 20)
const ptrBuf = new Uint8Array(8);
const ptrDv = new DataView(ptrBuf.buffer);
ptrDv.setBigUint64(0, BigInt(addr), true);
const ptrAddr = idPtr(ptrBuf);
check(read.ptr(ptrAddr, 0), addr, "read.ptr of a stored pointer");
check(read.intptr(ptrAddr, 0), addr, "read.intptr of a stored pointer");
// HOT: each reader through its own monomorphic caller to reach DFG/FTL, then compare to cold values.
for (const [t, off, expected] of cases) {
  const caller = new Function("f", "a", "o", "return f(a, o)");
  for (let i = 0; i < 4e5; i++) caller(read[t], addr, off);
  check(caller(read[t], addr, off), expected, "HOT read." + t + "@" + off);
}
// Address as BigInt round-trip and as int32-ish literal
check(read.u8(BigInt(addr), 0), dv.getUint8(0), "BigInt address");
// Offset argument omitted defaults to 0
check(read.u8(addr), dv.getUint8(0), "default byteOffset 0");
// Non-number address must throw
let threw = false; try { read.u8("not a pointer", 0); } catch (e) { threw = e instanceof TypeError; }
check(threw, true, "string address throws TypeError");
print("read.* : all checks passed (" + (cases.length * 2 + 8) + ")");
