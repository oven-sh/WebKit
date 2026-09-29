// The tsc workload, then the census of the CodeBlocks.
load("/workspace/wkbuild/workloads/tsc-workload.js");
if (typeof $vm !== "undefined" && typeof $vm.codeBlockCensus === "function")
    print("census " + JSON.stringify($vm.codeBlockCensus()));
