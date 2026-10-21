// Eden collections that mark little and have a lot of weak work for the constraint solver. usage: bun weak-heavy.mjs <targets|weakmaps> [iterations]
const kind = process.argv[2] ?? "targets", iterations = Number(process.argv[3] ?? 30);
let old = [];
if (kind === "weakmaps") { for (let i = 0; i < 200000; i++) { const wm = new WeakMap(); wm.set({}, i); old.push(wm); } Bun.gc(true); Bun.gc(true); }
const t0 = performance.now();
let keep;
for (let it = 0; it < iterations; it++) {
  if (kind === "targets") { const batch = new Array(143000); for (let i = 0; i < batch.length; i++) batch[i] = new EventTarget(); keep = batch; }
  else { const batch = new Array(150000); for (let i = 0; i < batch.length; i++) batch[i] = { i, a: [i] }; keep = batch; }
}
const ms = performance.now() - t0;
console.log(JSON.stringify({ kind, iterations, ms: +ms.toFixed(0), nsPerOp: +(ms * 1e6 / iterations / 143000).toFixed(1), hits: keep.length + old.length }));
