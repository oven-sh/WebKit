//@ skip
// semantics/termination-storm.js — SKIPPED: no $vm hook for VM-wide (or
// per-thread) termination exists in this tree.
//
// Checked 2026-06-07 against Source/JavaScriptCore/tools/JSDollarVM.cpp:
// no host function matching terminate/kill/abort/interrupt/trap is exposed
// (grep over JSC_DEFINE_HOST_FUNCTION / putDirectNativeFunction
// registrations), and jsc.cpp exposes no requestTermination()-style shell
// hook either. The only termination vector available to tests today is the
// watchdog (--watchdog=N --watchdog-exception-ok), which fires once,
// process-wide, on a timer — it cannot be aimed at the VM mid-run from JS,
// cannot be fired repeatedly, and cannot target a storm of N threads
// deterministically, so it cannot express this test.
//
// INTENDED TEST (write this when a hook lands, e.g. $vm.requestTermination()
// or a per-Thread terminate()): spawn N threads in bounded busy loops +
// parked Atomics.wait calls, fire VM-wide termination repeatedly while they
// run ("storm"), then assert: every thread observes the uncatchable
// TerminationException exactly once (finally blocks run, catch blocks do
// NOT retain it), parked waiters are kicked with the documented wait result,
// join() on each terminated thread reports the termination (not a hang and
// not a normal value), no thread keeps executing user code afterward, and
// the VM remains usable from the main thread.
//
// WOULD-FAIL-IF: n/a while skipped. Once enabled: VM-wide termination under
// N concurrent threads either misses a thread (a mutator keeps running user
// JS after termination was requested — the per-thread VMTraps fan-out lost
// a lite), double-delivers (a catch block observes and swallows the
// TerminationException, breaking its uncatchability), fails to kick parked
// Atomics/Condition waiters (join() hangs), or leaves the shared VM unusable
// for the surviving main thread.
