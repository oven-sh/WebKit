log.push(["waits before its await", $vm.asyncContext()]);
await gate;
log.push(["waits behind its await", $vm.asyncContext()]);
