// N threads of independent object creation + property adds; prints wall ms.
// usage: jsc scale-objadd.js -- N [iters]
const N = Number(arguments[0] || 1), ITERS = Number(arguments[1] || 2e6);
function work(iters) {
    function mk() { return {}; }
    let s = 0;
    for (let i = 0; i < iters; ++i) { const o = mk(); o.a = i; o.b = 1; o.c = 2; o.d = 3; s += o.d; }
    return s;
}
const t0 = preciseTime();
const threads = [];
for (let i = 1; i < N; ++i) threads.push(new Thread(work, ITERS));
work(ITERS);
for (const t of threads) t.join();
print("N=" + N, ((preciseTime() - t0) * 1000).toFixed(1), "ms");
