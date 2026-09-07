function makeAS(n) { const a = []; a[n + 100] = 1; a.length = 0; for (let i = 0; i < n; ++i) a.push(i); return a; } // ArrayStorage via sparse put
const n = 20000;
let a = makeAS(n);
if (typeof $vm !== "undefined" && !$vm.indexingMode(a).includes("ArrayStorage")) print("NOT AS: " + $vm.indexingMode(a));
const t0 = preciseTime();
let s = 0;
while (a.length) s += a.shift();
const t1 = preciseTime();
a = makeAS(1000);
for (let i = 0; i < 5000; ++i) { a.unshift(i); a.pop(); }
const t2 = preciseTime();
print("AS shift-drain 20k: " + ((t1 - t0) * 1000).toFixed(1) + " ms; unshift/pop 5k on 1k: " + ((t2 - t1) * 1000).toFixed(1) + " ms; sum=" + s);
