//@ requireOptions("--useVMLite=1")
// SPEC-vmstate I13: useVMLite=1 on a single thread is behavior-identical —
// Phase A VMLites are inert carriers (§6.1.4: the main carrier is created at
// the end of the VM ctor, installed by JSLock::didAcquireLock, and NOTHING
// in the interpreter/JIT/runtime reads it). The digest must equal the
// flags-off baseline exactly.
//
// NOTE (flag matrix): --useVMLite ships via INTEGRATE-vmstate M_opts (§3 R4
// orchestrator pre-apply). Until OptionsList.h carries it, this file cannot
// start; it is part of the integrated-tree matrix, not the pre-merge tree.
load("../resources/assert.js", "caller relative");
load("./resources/workload.js", "caller relative");

shouldBe(runVMStateWorkload(), VMSTATE_WORKLOAD_EXPECTED_DIGEST);
shouldBe(runVMStateWorkload(), VMSTATE_WORKLOAD_EXPECTED_DIGEST);

// I14 is exercised implicitly: every VM entry above passes
// VMEntryScope::setUpSlow, whose M13 debug assert checks the installed
// lite's vm matches the entered VM. A debug build failing here (or any
// I15/I18/I20 assert in VMLite.cpp/VM.h) is a test failure.
