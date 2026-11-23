log.push(["later before its await", $vm.asyncContext()]);
await laterGate;
log.push(["later behind its await", $vm.asyncContext()]);
