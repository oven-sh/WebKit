//@ runDefault("--returnEmptyBlocksAtEndOfCollection=1", "--retainedEmptyBlocksPerDirectory=0")
//@ runDefault("--returnEmptyBlocksAtEndOfCollection=1", "--retainedEmptyBlocksPerDirectory=1", "--useGenerationalGC=0")
//@ runDefault("--returnEmptyBlocksAtEndOfCollection=1", "--retainedEmptyBlocksPerDirectory=0", "--useConcurrentGC=0")

// Returning blocks that marking proved empty must never return one that still holds a reachable
// cell. Every survivor here is checked after the collection that could have freed its block.

function shouldBe(actual, expected) {
  if (actual !== expected) throw new Error(`bad value: expected ${expected} but got ${actual}`);
}

// Scattered survivors keep their blocks non-empty while the dropped majority empties others, so
// both the freed and the retained path run. A contiguous survivor run would only empty whole
// blocks and never exercise the partly-used case.
function churn(round) {
  const survivors = [];
  for (let i = 0; i < 20000; ++i) {
    const o = { round, i, payload: [i, i + 1, i + 2] };
    if (i % 10 === 0) survivors.push(o);
  }
  return survivors;
}

// Sustained churn with NO explicit gc() first: an explicit collection sweeps synchronously and
// shrinks, which returns the empty blocks before the End phase ever sees them. Only automatic,
// allocation-triggered collections drive the path under test.
const held = [];
for (let round = 0; round < 30; ++round) {
  const survivors = churn(round);
  for (let j = 0; j < survivors.length; ++j) shouldBe(survivors[j].i, j * 10);
  if (round % 10 === 0) held.push(survivors[0]);
}
for (let k = 0; k < held.length; ++k) shouldBe(held[k].round, k * 10);

const kept = [];
for (let round = 0; round < 20; ++round) {
  const survivors = churn(round);
  gc();
  for (let j = 0; j < survivors.length; ++j) {
    const o = survivors[j];
    shouldBe(o.round, round);
    shouldBe(o.i, j * 10);
    shouldBe(o.payload[2], j * 10 + 2);
  }
  // Hold every other round's survivors across the next round so blocks age past one cycle.
  if (round % 2 === 0) kept.push(survivors);
}

gc();
shouldBe(kept.length, 10);
for (let k = 0; k < kept.length; ++k) {
  const survivors = kept[k];
  shouldBe(survivors.length, 2000);
  shouldBe(survivors[0].round, k * 2);
  shouldBe(survivors[1999].i, 19990);
}

// Weak sets pin a block out of the eligible set (only a trivially destructible one may be freed
// unswept). Exercise that path: the referents must stay live and the finalizers must not be lost.
const registry = new FinalizationRegistry(function () {});
const weakRefs = [];
const referents = [];
for (let i = 0; i < 2000; ++i) {
  const o = { i };
  referents.push(o);
  weakRefs.push(new WeakRef(o));
  registry.register(o, i);
}
gc();
for (let i = 0; i < weakRefs.length; ++i) shouldBe(weakRefs[i].deref().i, i);
