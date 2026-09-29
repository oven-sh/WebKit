// The tsc workload, then the memory of the process. Usage: jsc --useDollarVM=1 rss-end.js -- 1 <small|full>
var __which = (typeof arguments !== "undefined" && arguments[1]) || "small";
load(__which === "full" ? "/workspace/wkbuild/workloads/tsc-workload.js" : "/workspace/wkbuild/workloads/tsc-small.js");
var __c = $vm.codeBlockCensus();
var __m = MemoryFootprint();
print("MEM " + JSON.stringify({ current: __m.current, peak: __m.peak, jitBytes: __c.jitBytes, baseline: __c.baseline }));
