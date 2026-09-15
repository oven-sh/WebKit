//@ runDefault("--maxPerThreadStackUsage=1000000", "--validateExceptionChecks=1")

// The imported module's default export is one left-deep chain of 20000 subtractions. The parser builds that chain
// without recursing, so the module parses and analyzes. The bytecode generator recurses once per operator, runs out
// of the stack this test allows and reports "Out of memory". The first thing to fail is therefore the link step, where
// ModuleProgramExecutable::tryCreate() generates the module's code. tryCreate() returned nullptr for that failure
// without checking the exception in its own scope, which exception check validation reports and aborts on.

function shouldThrowAsync(run, errorType, message) {
    let actual;
    var hadError = false;
    run().then(function(value) { actual = value; },
               function(error) { hadError = true; actual = error; });
    drainMicrotasks();

    if (!hadError)
        throw new Error("Expected " + run + "() to throw " + errorType.name + ", but did not throw.");
    if (!(actual instanceof errorType))
        throw new Error("Expected " + run + "() to throw " + errorType.name + ", but threw '" + actual + "'");
    if (message !== void 0 && actual.message !== message)
        throw new Error("Expected " + run + "() to throw '" + message + "', but threw '" + actual.message + "'");
}

shouldThrowAsync(async () => {
    await import("./resources/module-codegen-error-at-link/expression-too-deep.js");
}, RangeError, "Out of memory");
