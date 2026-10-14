// Bun: the stacks expected below take "the DFG is enabled" for "the DFG compiles baz()". The ftl-eager-no-cjit mode fails
// executable allocations at random, and a process in which those compilations fail only ever sees the bound function's frame.
//@ requireOptions("--useExecutableAllocationFuzz=false")
if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplingProfiler.js", "caller relative");

    function foo() {
        let o = {};
        for (let i = 0; i < 100; i++) {
            o[i + "p"] = i;
        }
    }

    function bar() {
        let o = {};
        for (let i = 0; i < 100; i++) {
            o[i + "p"] = i;
        }
    }

    let boundFoo = foo.bind(null);
    let boundBar = bar.bind(null);

    let baz = function() {
        boundFoo();
        boundBar();
    }

    // It depends on JIT enablement. But this is OK since this is sampling-profiler's internal data.
    if ($vm.useDFGJIT()) {
        runTest(baz, ["foo", "baz"]);
        runTest(baz, ["bar", "baz"]);
    } else {
        runTest(baz, ["foo", "bound foo", "baz"]);
        runTest(baz, ["bar", "bound bar", "baz"]);
    }
}
