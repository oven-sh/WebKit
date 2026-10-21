// Eden collections that each mark about S MB of wide, freshly allocated survivors. usage: bun bench-eden.mjs <S MB> [iterations]
const S = Number(process.argv[2] ?? 8);
const iterations = Number(process.argv[3] ?? 60);
const perArray = 64;
const arrays = Math.max(1, Math.floor((S * 1024 * 1024) / (perArray * 48)));
let keep = [];
const t0 = performance.now();
for (let it = 0; it < iterations; it++) {
  const batch = new Array(arrays);
  for (let i = 0; i < arrays; i++) {
    const arr = new Array(perArray);
    for (let j = 0; j < perArray; j++) arr[j] = { a: i, b: j };
    batch[i] = arr;
  }
  keep = batch;
}
const ms = performance.now() - t0;
const cpu = process.cpuUsage();
console.log(JSON.stringify({ S, iterations, ms: +ms.toFixed(0), nsPerOp: +(ms * 1e6 / (iterations * arrays * perArray)).toFixed(1), userS: +(cpu.user / 1e6).toFixed(2), systemS: +(cpu.system / 1e6).toFixed(2), keep: keep.length }));
