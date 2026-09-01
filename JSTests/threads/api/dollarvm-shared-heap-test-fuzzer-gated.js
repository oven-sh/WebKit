//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--useFuzzerMode=1")
// $vm.sharedHeapTest spawns raw threads and RELEASE_ASSERTs on misuse (for
// example when a spawned Thread calls it under gilOff), so like $vm.crash it
// must be withheld under useFuzzerMode, while always-safe entries such as
// $vm.gc stay exposed.
if (typeof $vm.sharedHeapTest !== "undefined")
    throw new Error("$vm.sharedHeapTest must not be exposed under --useFuzzerMode=1");
if (typeof $vm.crash !== "undefined")
    throw new Error("$vm.crash must not be exposed under --useFuzzerMode=1");
if (typeof $vm.gc !== "function")
    throw new Error("$vm.gc must stay exposed under --useFuzzerMode=1");
