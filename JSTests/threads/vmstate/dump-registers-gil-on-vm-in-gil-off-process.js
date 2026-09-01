//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--useDollarVM=1")
// In a GIL-off process only the first VM wins the shared-server designation and
// runs gilOff; a second VM (the shell's $.agent runs one on its own thread)
// keeps its GIL and is entered through its main carrier, so its top call frame
// and stack bounds live in the VM block while that carrier's primitives stay
// zero. VMInspector resolves a frame of either VM through the VM's own storage
// selector, so $vm.dumpRegisters() on the second VM walks and prints the frame
// instead of refusing with "Cannot find callFrame on any VM stack.".
//
// The dump and the refusal both go to the data log (stderr), which the test
// cannot read back; this file drives the path on both VM shapes and checks
// that neither crashes or hangs. Run it by hand to see the two register tables.

function dumpHere() {
    $vm.dumpRegisters();
    $vm.dumpRegisters(1);
    return 1;
}
noInline(dumpHere);

if (dumpHere() !== 1)
    throw new Error("dumpHere did not return on the designation winner");

$.agent.start(`
    function dumpHere() {
        $vm.dumpRegisters();
        $vm.dumpRegisters(1);
        return 1;
    }
    noInline(dumpHere);
    $.agent.report(dumpHere() === 1 ? "done" : "bad-return");
`);

let report;
while (!(report = $.agent.getReport())) {}
if (report !== "done")
    throw new Error("unexpected agent report: " + report);
