//@ requireOptions("--useJSThreads=1")
// SPEC-vmstate I13/I14: useJSThreads=1 implies all three §3 flags (M_opts2,
// R2: useSharedAtomStringTable + useVMLite + useStructureAllocationLock).
// The shared workload must produce the baseline digest on the main thread
// AND inside every spawned Thread (GIL phase: threads enter the VM through
// JSLock, which installs/restores VMLite carriers per §6.4.4 — M4).
//
// THREADS-INTEGRATE(vmstate): Thread/join come from SPEC-api's GIL stub.
load("../resources/assert.js", "caller relative");
load("./resources/workload.js", "caller relative");

// Main thread first (covers the main carrier, tid 0).
shouldBe(runVMStateWorkload(), VMSTATE_WORKLOAD_EXPECTED_DIGEST);

// Then concurrently from spawned threads: each thread runs the same
// exception/stack/regexp/microtask/structure workload. Identical digests
// prove per-thread execution state never bleeds across threads (Group 2/3
// state is per-JSLock-hold under the GIL; I15 asserts back this in debug).
const THREADS = 3;
const digests = joinAll(spawnN(THREADS, () => runVMStateWorkload()));
for (let t = 0; t < THREADS; ++t)
    shouldBe(digests[t], VMSTATE_WORKLOAD_EXPECTED_DIGEST, "thread " + t);

// And once more on the main thread after the threads died: thread teardown
// (lite unregister + setCurrent(null) under the final JSLock hold, §6.5.1
// N8) must leave the main thread's state untouched.
shouldBe(runVMStateWorkload(), VMSTATE_WORKLOAD_EXPECTED_DIGEST);
