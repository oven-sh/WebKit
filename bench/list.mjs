// Full collection of a linked list of N nodes: best and median of 7 Bun.gc(true). usage: bun list.mjs [nodes]
const N = Number(process.argv[2] ?? 2_000_000);
let head = null;
for (let i = 0; i < N; i++) head = { next: head, i };
globalThis.head = head;
Bun.gc(true);
const times = [];
for (let i = 0; i < 7; i++) { const t0 = performance.now(); Bun.gc(true); times.push(performance.now() - t0); }
times.sort((a, b) => a - b);
console.log(JSON.stringify({ nodes: N, bestMs: +times[0].toFixed(1), medMs: +times[3].toFixed(1), nsPerOp: +times[0].toFixed(1), hits: N }));
