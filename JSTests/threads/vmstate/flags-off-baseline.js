// SPEC-vmstate I4/R3/I13 baseline: with ALL §3 flags off (the default), the
// shared workload produces this exact digest. The flag-on variants
// (vmlite-single-thread-identity.js, all-flags-identity.js) assert the SAME
// constant — together they are the JS-level behavior-identity gate of the
// flag matrix (see README.md).
//
// No //@ requireOptions: this runs in today's tree with no thread flags at
// all and must keep passing unchanged forever (R3: flag-off => behavioral
// identity; only codegen deltas R3(a)-(d) are permitted, none observable
// from JS).
load("../resources/assert.js", "caller relative");
load("./resources/workload.js", "caller relative");

shouldBe(runVMStateWorkload(), VMSTATE_WORKLOAD_EXPECTED_DIGEST);

// Run it twice: warmed-up paths (ICs, compiled regexp, cached structures)
// must not change behavior either.
shouldBe(runVMStateWorkload(), VMSTATE_WORKLOAD_EXPECTED_DIGEST);
