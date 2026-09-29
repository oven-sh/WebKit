// TODO(bun): globalFuncImportModule() still wraps the loader's promise under USE(BUN_JSC_ADDITIONS) and, while that promise
// is pending, resolves the wrapper with it through the ordinary resolve(), which looks up Promise.prototype.then. The
// wrapper's other branch (the loader's promise is already fulfilled, 8a5ce3999589) cannot be taken since the module loader
// rewrite (4a638109b905): requestImportModule() always returns a pending promise. Return the loader's promise as upstream
// does, and if import() of an evaluated module should settle without a tick again, do that in requestImportModule().
//@ skip
//@ runDefault

// Test that replacing Promise.prototype.then with a function that calls resolve
// with a wrong value does NOT affect module loading. The module loader uses
// resolveIgnoringThenable which bypasses thenable unwrapping entirely.
//
// Previously, this would cause a type confusion crash because
// hostLoadImportedModule resolves the fetchPromise with another JSPromise
// (the fetch result). With resolveIgnoringThenable, the fetch result promise
// is piped directly via internal microtask, completely bypassing user-observable
// Promise.prototype.then.

var originalThen = Promise.prototype.then;
Promise.prototype.then = function (onFulfilled, onRejected) {
    onFulfilled("tampered");
};

originalThen.call(
    import("./resources/module-loader-promise-then-tampered-target.js"),
    function (ns) {
        if (ns.value !== 42) {
            print("Expected ns.value to be 42, got " + ns.value);
            $vm.abort();
        }
        if (ns.dep !== "from dependency") {
            print("Expected ns.dep to be 'from dependency', got " + ns.dep);
            $vm.abort();
        }
    },
    function (e) {
        print("Module loading should not have failed: " + e);
        $vm.abort();
    }
);
