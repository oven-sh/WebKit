export function then(resolve) {
    log.push(["namespace then", $vm.asyncContext()]);
    resolve("thenable");
}
