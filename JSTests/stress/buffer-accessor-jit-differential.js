//@ requireOptions("--useDollarVM=1")

class Buffer extends Uint8Array {}
Object.assign(Buffer.prototype, $vm.createBufferAccessors());

let seed = 0x9e3779b1;
function rand() {
  seed ^= seed << 13;
  seed |= 0;
  seed ^= seed >>> 17;
  seed ^= seed << 5;
  seed |= 0;
  return (seed >>> 0) / 4294967296;
}
function pick(list) {
  return list[(rand() * list.length) | 0];
}
function randInt(lowInclusive, highInclusive) {
  return lowInclusive + ((rand() * (highInclusive - lowInclusive + 1)) | 0);
}

const accessorNames = Object.keys($vm.createBufferAccessors());
const readers = accessorNames.filter(n => n.startsWith("read"));
const writers = accessorNames.filter(n => n.startsWith("write"));

function describe(name) {
  const isWrite = name.startsWith("write");
  const isFloat = /Float/.test(name);
  const isDouble = /Double/.test(name);
  const isBigInt = /Big/.test(name);
  const isVarWidth = /Int(LE|BE)$/.test(name) && !/(8|16|32|64)/.test(name);
  const isSigned = !/UInt/.test(name);
  let byteSize = isDouble ? 8 : isFloat ? 4 : isBigInt ? 8 : isVarWidth ? 0 : Number(name.match(/(8|16|32|64)/)[0]) / 8;
  return { isWrite, isFloat: isFloat || isDouble, isBigInt, isVarWidth, isSigned, byteSize };
}

function makeInvoker(name, arm) {
  if (!/^[A-Za-z0-9_]+$/.test(name)) throw new Error("unexpected accessor name: " + name);
  const source =
    "return function invoke_" + arm + "_" + name + "(receiver, args, box) {" +
    "  try {" +
    "    let result;" +
    "    switch (args.length) {" +
    "    case 0: result = receiver." + name + "(); break;" +
    "    case 1: result = receiver." + name + "(args[0]); break;" +
    "    case 2: result = receiver." + name + "(args[0], args[1]); break;" +
    "    default: result = receiver." + name + "(args[0], args[1], args[2]); break;" +
    "    }" +
    "    box.value = result; box.error = null;" +
    "  } catch (e) {" +
    "    box.value = undefined;" +
    "    box.error = e === null || typeof e !== 'object'" +
    "      ? 'throw:' + String(e)" +
    "      : 'throw:' + e.constructor.name + ':' + (e.code === undefined ? '' : e.code) + ':' + e.message;" +
    "  }" +
    "};";
  return new Function(source)();
}

const invokerPairs = new Map();
function pairFor(name, key) {
  let pair = invokerPairs.get(key);
  if (!pair) {
    const jitInvoke = makeInvoker(name, "jit");
    const refInvoke = makeInvoker(name, "ref");
    noDFG(refInvoke);
    noFTL(refInvoke);
    pair = { jitInvoke, refInvoke };
    invokerPairs.set(key, pair);
  }
  return pair;
}

function cleanValue(shape, byteSize) {
  if (shape.isBigInt) {
    const bits = shape.isSigned
      ? [-(2n ** 63n), 2n ** 63n - 1n, 0n, -1n, BigInt(randInt(-1e6, 1e6))]
      : [0n, 2n ** 64n - 1n, 12345678901234567890n, BigInt(randInt(0, 1e6))];
    return pick(bits);
  }
  if (shape.isFloat)
    return pick([
      () => rand() * 1e6 - 5e5,
      () => Math.fround(rand() * 100),
      () => -0,
      () => Infinity,
      () => 2 ** -1074,
      () => 1e300,
    ])();
  const size = shape.isVarWidth ? byteSize : shape.byteSize;
  const min = shape.isSigned ? -(2 ** (8 * size - 1)) : 0;
  const max = shape.isSigned ? 2 ** (8 * size - 1) - 1 : 2 ** (8 * size) - 1;
  return pick([
    () => min,
    () => max,
    () => randInt(min, max),
    () => randInt(min, max),
    () => randInt(min, max),
    () => 0,
  ])();
}

function dirtyValue() {
  return pick([
    () => (rand() * 2 ** 32) | 0,
    () => -((rand() * 2 ** 31) | 0),
    () => 2 ** 31,
    () => 2 ** 32,
    () => -(2 ** 31) - 1,
    () => rand() * 1e6 - 5e5,
    () => 0.5,
    () => -0.5,
    () => -0,
    () => NaN,
    () => Infinity,
    () => -Infinity,
    () => 2 ** 53 + 1,
    () => "42",
    () => "abc",
    () => "",
    () => true,
    () => false,
    () => null,
    () => undefined,
    () => 5n,
    () => 2n ** 63n,
    () => 2n ** 64n,
    () => -1n,
    () => -(2n ** 63n) - 1n,
    () => Symbol("v"),
  ])();
}

function dirtyOffset(length) {
  return pick([
    () => randInt(0, length + 3),
    () => -randInt(1, 8),
    () => length - randInt(0, 8),
    () => rand() * length,
    () => -0,
    () => 2 ** 31 + randInt(0, 8),
    () => 2 ** 32,
    () => 2 ** 53 + 2,
    () => NaN,
    () => Infinity,
    () => -Infinity,
    () => undefined,
    () => null,
    () => String(randInt(0, length)),
    () => "not a number",
    () => true,
    () => Symbol("s"),
    () => 3n,
  ])();
}

function dirtyByteLength() {
  return pick([
    () => randInt(1, 6),
    () => 0,
    () => 7,
    () => -1,
    () => 2.5,
    () => NaN,
    () => "4",
    () => undefined,
    () => 9n,
  ])();
}

function makeReceiverFactory() {
  return pick([
    () => new Buffer(32),
    () => new Buffer(new ArrayBuffer(64), 8, 24),
    () => new Buffer(7),
    () => new Buffer(new ArrayBuffer(16, { maxByteLength: 64 })),
    () => new Buffer(new ArrayBuffer(48, { maxByteLength: 64 }), 8, 16),
    () => new Buffer(new SharedArrayBuffer(32, { maxByteLength: 64 })),
  ]);
}

function sameOutcome(jitBox, refBox) {
  if (jitBox.error !== refBox.error) return false;
  if (jitBox.error !== null) return true;
  return (
    Object.is(jitBox.value, refBox.value) ||
    (typeof jitBox.value === "bigint" && jitBox.value === refBox.value) ||
    (Number.isNaN(jitBox.value) && Number.isNaN(refBox.value))
  );
}
function sameBytes(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; ++i) if (a[i] !== b[i]) return false;
  return true;
}

const rounds = 200;
const opsPerRound = 1500;
let mismatch = null;
let cleanOps = 0,
  dirtyOps = 0;

for (let round = 0; round < rounds && !mismatch; ++round) {
  const factory = makeReceiverFactory();
  const jitReceiver = factory();
  const refReceiver = factory();
  if (jitReceiver.length !== refReceiver.length || jitReceiver.buffer.constructor !== refReceiver.buffer.constructor)
    continue;
  const name = pick(rand() < 0.5 ? readers : writers);
  const shape = describe(name);
  const clean = rand() < 0.6;
  const { jitInvoke, refInvoke } = pairFor(name, name + (clean ? ":clean" : ":dirty"));
  const jitBox = { value: undefined, error: null };
  const refBox = { value: undefined, error: null };
  let resizeCountdown = clean ? Infinity : 100 + randInt(0, 400);
  const width = shape.isVarWidth ? randInt(1, 6) : shape.byteSize;

  for (let step = 0; step < opsPerRound; ++step) {
    const args = [];
    const maxOffset = jitReceiver.length - width;
    if (clean) {
      if (maxOffset < 0) break;
      if (shape.isWrite) args.push(cleanValue(shape, width));
      args.push(randInt(0, maxOffset));
      if (shape.isVarWidth) args.push(width);
      cleanOps++;
    } else {
      if (shape.isWrite) args.push(dirtyValue());
      if (rand() < 0.9 || shape.isVarWidth) args.push(dirtyOffset(jitReceiver.length));
      if (shape.isVarWidth) args.push(dirtyByteLength());
      while (args.length && args[args.length - 1] === undefined && rand() < 0.3) args.pop();
      if (args.length && typeof args[args.length - 1] === "symbol" && rand() < 0.5) args[args.length - 1] = 0;
      dirtyOps++;
    }

    jitInvoke(jitReceiver, args, jitBox);
    refInvoke(refReceiver, args, refBox);

    if (!sameOutcome(jitBox, refBox)) {
      mismatch = {
        round,
        step,
        name,
        clean,
        args,
        jit: jitBox.error === null ? String(jitBox.value) : jitBox.error,
        ref: refBox.error === null ? String(refBox.value) : refBox.error,
      };
      break;
    }
    if (!sameBytes(jitReceiver, refReceiver)) {
      mismatch = {
        round,
        step,
        name,
        clean,
        args,
        jit: "bytes:" + Array.from(jitReceiver).join(","),
        ref: "bytes:" + Array.from(refReceiver).join(","),
      };
      break;
    }

    if (--resizeCountdown === 0) {
      resizeCountdown = 100 + randInt(0, 400);
      const jb = jitReceiver.buffer,
        rb = refReceiver.buffer;
      if (typeof jb.resize === "function" && jb.resizable) {
        const size = randInt(0, jb.maxByteLength);
        try {
          jb.resize(size);
          rb.resize(size);
        } catch {}
      } else if (typeof jb.grow === "function" && jb.growable) {
        const size = randInt(jb.byteLength, jb.maxByteLength);
        try {
          jb.grow(size);
          rb.grow(size);
        } catch {}
      } else if (rand() < 0.15) {
        try {
          structuredClone(jb, { transfer: [jb] });
          structuredClone(rb, { transfer: [rb] });
        } catch {}
      }
    }
  }
}

if (mismatch) {
  const shown = mismatch.args.map(a =>
    typeof a === "bigint"
      ? a.toString() + "n"
      : typeof a === "symbol"
        ? "Symbol"
        : typeof a === "object" && a !== null
          ? "{obj}"
          : String(a),
  );
  throw new Error(
    "differential mismatch (seed 0x9e3779b1): round " +
      mismatch.round +
      " step " +
      mismatch.step +
      " " +
      mismatch.name +
      (mismatch.clean ? " [clean]" : " [dirty]") +
      "(" +
      shown.join(", ") +
      ") jit=" +
      mismatch.jit +
      " ref=" +
      mismatch.ref,
  );
}
if (cleanOps < 50000 || dirtyOps < 20000)
  throw new Error("fuzzer under-covered: cleanOps=" + cleanOps + " dirtyOps=" + dirtyOps);
