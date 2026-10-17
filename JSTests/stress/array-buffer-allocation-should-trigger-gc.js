//@ runDefault("--useConcurrentGC=0")
let startTime = $vm.totalGCTime();

let keep = new Array(1024).fill(null);
for (let i = 0; i < 6 * 1024; ++i)
    keep[i & 1023] = new ArrayBuffer(8 * 1024);

if ($vm.totalGCTime() - startTime == 0)
    throw new Error("GC did not run");
