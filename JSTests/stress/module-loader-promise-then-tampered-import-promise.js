//@ runDefault

// The promise that import() returns must settle with the loader's outcome even
// when Promise.prototype.then has been replaced: ContinueDynamicImport only ever
// settles it through its PromiseCapability, so nothing on the path may look up
// "then" on a %Promise%. module-loader-promise-then-tampered.js covers a module
// with a dependency; this covers a module without one and a fetch that fails,
// which must reject rather than fulfill with whatever the replaced then passes.

var originalThen = Promise.prototype.then;
Promise.prototype.then = function (onFulfilled, onRejected) {
    onFulfilled("tampered");
};

var remaining = 2;
function done() {
    if (--remaining === 0)
        Promise.prototype.then = originalThen;
}

originalThen.call(
    import("./resources/module-loader-promise-then-tampered-no-deps.js"),
    function (ns) {
        done();
        if (typeof ns !== "object" || ns.value !== 42) {
            print("Expected the no-deps namespace with value 42, got " + String(ns));
            $vm.abort();
        }
    },
    function (e) {
        done();
        print("Loading the no-deps module should not have failed: " + e);
        $vm.abort();
    }
);

originalThen.call(
    import("./resources/module-loader-promise-then-tampered-missing.js"),
    function (ns) {
        done();
        print("Importing a missing module should have rejected, got " + String(ns));
        $vm.abort();
    },
    function (e) {
        done();
        if (!(e instanceof Error)) {
            print("Expected an Error for the missing module, got " + String(e));
            $vm.abort();
        }
    }
);
