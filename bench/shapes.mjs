// Full collections of a ~100 MB heap of one shape: best and median of 7 Bun.gc(true). usage: bun shapes.mjs <wide|records|map|tree|list> [MB]
const shape = process.argv[2] ?? "wide", MBs = Number(process.argv[3] ?? 100);
let keep;
if (shape === "wide") { const n = Math.floor(MBs * 1048576 / 48 / 64); keep = Array.from({ length: n }, (_, i) => Array.from({ length: 64 }, (_, j) => ({ a: i, b: j }))); }
else if (shape === "records") { const n = Math.floor(MBs * 1048576 / 780); keep = Array.from({ length: n }, (_, i) => ({ id: i, name: "user" + i, email: "user" + i + "@example.com", tags: ["t" + (i % 13), "u" + (i % 7)], address: { street: i + " main st", city: "city" + (i % 50), geo: { lat: i / 7, lng: i / 3 } }, orders: Array.from({ length: 4 }, (_, j) => ({ sku: "sku" + ((i * 4 + j) % 997), qty: j + 1, price: j * 1.5 })) })); }
else if (shape === "map") { const n = Math.floor(MBs * 1048576 / 110); keep = new Map(); for (let i = 0; i < n; i++) keep.set(i, { key: i, value: "v" + i }); }
else if (shape === "tree") { const depth = Math.floor(Math.log2(MBs * 1048576 / 48)); const build = d => d ? { l: build(d - 1), r: build(d - 1), d } : null; keep = build(depth); }
else if (shape === "list") { const n = Math.floor(MBs * 1048576 / 32); let head = null; for (let i = 0; i < n; i++) head = { next: head, i }; keep = head; }
globalThis.keep = keep;
Bun.gc(true);
const times = [];
for (let i = 0; i < 7; i++) { const t0 = performance.now(); Bun.gc(true); times.push(performance.now() - t0); }
times.sort((a, b) => a - b);
console.log(JSON.stringify({ shape, heapMB: +(process.memoryUsage().heapUsed / 1048576).toFixed(0), bestMs: +times[0].toFixed(1), medMs: +times[3].toFixed(1), nsPerOp: +times[3].toFixed(1), hits: 1 }));
