//@ requireOptions("--useDollarVM=1")

// Detached, shrunk, regrown and out-of-bounds receivers at call sites that are already JIT-compiled,
// including detaching / resizing from inside the value argument's valueOf: every access must land on
// the live length (throw) and never touch the old storage.
const accessors = $vm.createBufferAccessors();
class Buffer extends Uint8Array {}
Object.assign(Buffer.prototype, accessors);
function expectThrow(f, what) { try { f(); } catch (e) { return; } throw new Error("expected throw: " + what); }

function rd(b, o) { return b.readInt32LE(o); }
function rd8(b, o) { return b.readDoubleBE(o); }
function rdb(b, o) { return b.readBigUInt64LE(o); }
function wr(b, v, o) { return b.writeUInt32LE(v, o); }
function wr8(b, v, o) { return b.writeInt8(v, o); }
function wrd(b, v, o) { return b.writeDoubleLE(v, o); }
function wrb(b, v, o) { return b.writeBigInt64LE(v, o); }
for (const f of [rd, rd8, rdb, wr, wr8, wrd, wrb]) noInline(f);

function warm(b) {
  for (let i = 0; i < testLoopCount; i++) { rd(b, i & 31); rd8(b, i & 31); rdb(b, i & 31); wr(b, i, i & 31); wr8(b, i & 127, i & 31); wrd(b, i, i & 31); wrb(b, 1n, i & 31); }
}

// 1. detach after tier-up, same call sites
{
  const b = new Buffer(64); warm(b);
  transferArrayBuffer(b.buffer);
  for (let i = 0; i < 100; i++) {
    expectThrow(() => rd(b, 0), "rd detached"); expectThrow(() => rd8(b, 0), "rd8"); expectThrow(() => rdb(b, 0), "rdb");
    expectThrow(() => wr(b, 1, 0), "wr"); expectThrow(() => wr8(b, 1, 0), "wr8"); expectThrow(() => wrd(b, 1, 0), "wrd"); expectThrow(() => wrb(b, 1n, 0), "wrb");
  }
}
// 2. detach from inside value.valueOf while the call site is hot
{
  const b = new Buffer(64); warm(b);
  const evil = { valueOf() { transferArrayBuffer(b.buffer); return 7; } };
  expectThrow(() => wr(b, evil, 0), "wr valueOf-detach");
  const b2 = new Buffer(64); warm(b2);
  const evil2 = { valueOf() { transferArrayBuffer(b2.buffer); return 7; } };
  expectThrow(() => wr8(b2, evil2, 60), "wr8 valueOf-detach");
  expectThrow(() => wrd(b2, 1, 0), "after");
}
// 3. resizable: shrink from inside valueOf, and shrink between hot calls
{
  const rab = new ArrayBuffer(64, { maxByteLength: 4096 });
  const b = new Buffer(rab); warm(b);          // length-tracking view
  rab.resize(8);
  expectThrow(() => rd(b, 5), "rd past shrunk end"); rd(b, 4);
  expectThrow(() => wrd(b, 1, 1), "wrd past shrunk end"); wr8(b, 5, 7); expectThrow(() => wr8(b, 5, 8), "wr8 at shrunk length");
  const evil = { valueOf() { rab.resize(2); return 1; } };
  expectThrow(() => wr(b, evil, 0), "wr valueOf-shrink");
  rab.resize(0);
  expectThrow(() => rd(b, 0), "rd zero-length"); expectThrow(() => wr8(b, 1, 0), "wr8 zero-length");
  rab.resize(4096); if (wr(b, 0xdeadbeef, 4092) !== 4096 || rd(b, 4092) !== (0xdeadbeef | 0)) throw new Error("regrow");
  // fixed-length view on a resizable buffer that shrinks below the view -> view goes out of bounds (length 0)
  const fixed = new Buffer(rab, 16, 64); warm(fixed);
  rab.resize(20);
  expectThrow(() => rd(fixed, 0), "fixed view OOB after shrink"); expectThrow(() => wr8(fixed, 1, 0), "fixed view OOB write");
}
// 4. growable shared, length-tracking
{
  const gsab = new SharedArrayBuffer(16, { maxByteLength: 256 });
  const b = new Buffer(gsab); warm(new Buffer(64));
  expectThrow(() => rd(b, 13), "gsab oob");
  gsab.grow(256); if (wr(b, 123, 252) !== 256 || rd(b, 252) !== 123) throw new Error("gsab grown");
}
// 5. polymorphic site: hot on Buffer, then detached plain Uint8Array / other receivers
{
  const b = new Buffer(64); warm(b);
  const u = new Uint8Array(16); transferArrayBuffer(u.buffer);
  for (let i = 0; i < testLoopCount / 10; i++) { expectThrow(() => rd(u, 0), "plain detached"); rd(b, i & 31); }
}

