//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=1", "--useCodeRecoveryFromBytecodeCache=0")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=1", "--useRunOnceCodeRelease=0")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=1", "--useLeanBytecodeCacheDecoder=0")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=0")
//@ runBytecodeCache("--useLazyModuleFunctionDeclarations=1")

// A module's function declarations that nobody has read yet stay in the bytecode cache payload
// (useLazyModuleFunctionDeclarations), and the module's code is let go of once its body has run, the unlinked code too
// when it can be decoded again from a persistent payload (useRunOnceCodeRelease, useCodeRecoveryFromBytecodeCache). The
// module record must not be what keeps the UnlinkedModuleProgramCodeBlock alive for the sake of those declarations,
// and reading one later has to work without it.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const options = jscOptions();
const staysInterpreted = options.useLLInt && options.thresholdForJITAfterWarmUp >= 100 && options.thresholdForJITSoon >= 100;
const releasesUnlinkedCode = staysInterpreted && options.useRunOnceCodeRelease && options.useCodeRecoveryFromBytecodeCache && options.useLeanBytecodeCacheDecoder
    && options.useBorrowedBytecodeFromCache && options.diskCachePayloadIsPersistentForTesting && options.forceDiskCache;

async function test() {
    const lib = await import("./resources/module-lazy-declarations-released-code/lib.js");
    assert(lib.marker === 11, "the body ran: " + lib.marker);
    fullGC();
    const uninstantiated = $vm.uninstantiatedFunctionDeclarations(lib);
    if (options.useLazyModuleFunctionDeclarations)
        assert(uninstantiated >= 6, "declarations nobody read are not instantiated: " + uninstantiated);
    else
        assert(!uninstantiated, "every declaration is instantiated: " + uninstantiated);
    const census = $vm.codeBlockCensus();
    if (releasesUnlinkedCode)
        assert(census.unlinkedModule === 0, "the module's unlinked code is gone although declarations are uninstantiated: " + JSON.stringify(census));

    // Reading them now decodes them without the code block.
    assert(lib.neverReadUntilLate(4) === 112, "neverReadUntilLate");
    fullGC();
    assert(lib.alsoLate(1, 2) === "2:3", "alsoLate");
    assert([...lib.lateGenerator(4)].join() === "0,1,4,9", "lateGenerator");
    assert(await lib.lateAsync(Promise.resolve(41)) === 42, "lateAsync");
    assert(lib.callCount() === 4, "callCount " + lib.callCount());
    assert(lib.neverReadUntilLate.name === "neverReadUntilLate" && lib.alsoLate.length === 2, "name and length");
    assert(lib.alsoLate.toString().startsWith("function alsoLate(a, b)"), "toString");
    if (options.useLazyModuleFunctionDeclarations)
        assert($vm.uninstantiatedFunctionDeclarations(lib) < uninstantiated, "reads instantiated them");
    if (releasesUnlinkedCode) {
        fullGC();
        assert($vm.codeBlockCensus().unlinkedModule === 0, "and did not bring the module's unlinked code back");
    }
    const again = await import("./resources/module-lazy-declarations-released-code/lib.js");
    assert(again === lib && lib.neverReadUntilLate === again.neverReadUntilLate, "same namespace, same function");
}

test().then(() => { }, (e) => { print("FAIL: " + e + "\n" + e.stack); $vm.abort(); });
