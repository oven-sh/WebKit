log.push(["tla before await", $vm.asyncContext()]);
await null;
log.push(["tla after await", $vm.asyncContext()]);
