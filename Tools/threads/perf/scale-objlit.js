// N threads of independent object-literal creation (sinkable in FTL); prints wall ms.
const N = Number(arguments[0] || 1), ITERS = Number(arguments[1] || 4e6);
function work(iters) {
    let s = 0;
    for (let i = 0; i < iters; ++i) { const o = { a: i, b: 1, c: 2, d: 3 }; s += o.d + o.a; }
    return s;
}
const t0 = preciseTime();
const threads = [];
for (let i = 1; i < N; ++i) threads.push(new Thread(work, ITERS));
work(ITERS);
for (const t of threads) t.join();
print("N=" + N, ((preciseTime() - t0) * 1000).toFixed(1), "ms");
