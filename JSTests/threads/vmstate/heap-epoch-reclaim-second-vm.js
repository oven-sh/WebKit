//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--useSharedGCHeap=0")
// The epochReclaim harness scenario observes collections through a
// Heap::addStopTheWorldSafepointHook hook. The hook registry is per-Heap, so
// the scenario must register on every heap it runs against: once the main
// VM's heap has run it, a second VM's heap (the jsc shell's $.agent owns one)
// must still see its own stops, or its RELEASE_ASSERT on the observation
// fires. --useSharedGCHeap=0 is pinned so both VMs stay in the 1-client !ISS
// configuration the scenario requires.
if (typeof $vm === "undefined" || typeof $vm.sharedHeapTest !== "function")
    throw new Error("needs --useDollarVM=1 with $vm.sharedHeapTest");

function runOnSecondVM() {
    $.agent.start(`
        $.agent.receiveBroadcast((sab) => {
            let ok = false;
            try {
                ok = $vm.sharedHeapTest("epochReclaim", 1, 16);
            } catch (e) {
            }
            $.agent.report(ok ? "agent-pass" : "agent-fail");
        });
    `);
    $.agent.broadcast(new SharedArrayBuffer(8));
    let report;
    while (!(report = $.agent.getReport())) {}
    return report;
}

function main() {
    if (!$vm.sharedHeapTest("epochReclaim", 1, 16))
        throw new Error("epochReclaim failed on the main VM's heap");

    let report = runOnSecondVM();
    if (report !== "agent-pass")
        throw new Error("epochReclaim on a second VM's heap: " + report);

    // The main heap still observes its own collections after another heap ran.
    if (!$vm.sharedHeapTest("epochReclaim", 1, 8))
        throw new Error("epochReclaim failed on the main VM's heap after the second VM ran");
}

main();
