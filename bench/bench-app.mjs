// App-like churn: batches of JSON-like records (nested objects, arrays, strings) in a sliding window.
// usage: bun bench-app.mjs [records per batch] [window batches] [iterations]
const PER = Number(process.argv[2] ?? 4000), WINDOW = Number(process.argv[3] ?? 8), ITER = Number(process.argv[4] ?? 400);
const makeBatch = (b) => {
  const out = new Array(PER);
  for (let i = 0; i < PER; i++) {
    out[i] = {
      id: b * PER + i, name: "user" + i, email: "user" + i + "@example.com",
      tags: ["t" + (i % 13), "u" + (i % 7), "v" + (i % 3)],
      address: { street: i + " main st", city: "city" + (i % 50), geo: { lat: i / 7, lng: i / 3 } },
      orders: Array.from({ length: 4 }, (_, j) => ({ sku: "sku" + ((i * 4 + j) % 997), qty: j + 1, price: j * 1.5 })),
    };
  }
  return out;
};
const window = [];
let checksum = 0;
const cpu0 = process.cpuUsage(), t0 = performance.now();
for (let it = 0; it < ITER; it++) {
  const batch = makeBatch(it);
  window.push(batch);
  if (window.length > WINDOW) window.shift();
  // read some of it back, like a handler would
  const s = JSON.stringify(batch[it % PER]);
  checksum += s.length + JSON.parse(s).orders.length;
}
const ms = performance.now() - t0, cpu = process.cpuUsage(cpu0);
console.log(JSON.stringify({ runtime: typeof Bun !== "undefined" ? "bun" : "node", hits: checksum, nsPerOp: +(ms * 1e6 / (ITER * PER)).toFixed(1), wallS: +(ms / 1000).toFixed(2), userS: +(cpu.user / 1e6).toFixed(2), systemS: +(cpu.system / 1e6).toFixed(2) }));
