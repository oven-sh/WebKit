//@ requireOptions("--forceDiskCache=0")

// The imported module holds a BigInt literal of about 1.05 million bits. That
// used to exceed maxLengthBits (1 << 20) and made module codegen throw. The
// limit is 1 << 30 bits now, and a checked-in source file cannot reasonably
// exceed it, so the import must succeed instead.
function shouldResolve(run, check) {
    let actual;
    var hadError = false;
    run().then(function(value) { actual = value; },
               function(error) { hadError = true; actual = error; });
    drainMicrotasks();

    if (hadError)
        throw new Error("Expected " + run + "() to resolve, but threw '" + actual + "'");
    check(actual);
}

shouldResolve(async () => {
    return await import("./import-tests/bigint-oom.js")
}, (module) => {
    if (typeof module.default !== "bigint")
        throw new Error("Expected a bigint, got " + typeof module.default);
    if (module.default >> 1048575n === 0n)
        throw new Error("Imported bigint is smaller than expected");
});