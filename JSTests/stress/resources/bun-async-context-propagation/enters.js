import "./enters-dependency.js";

$vm.setAsyncContext(entered);
log.push(["enters", $vm.asyncContext()]);
