//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useCodeRecoveryFromBytecodeCache=0")

// VM::persistentBytecodePayloads() keeps a slot per payload and SourceProvider for as long as code decoded from it, or a
// Decoder for it, is alive, and no longer: a program that loads the same file again and again (each load is a new
// SourceProvider around the same bytes) does not accumulate them, nor the providers and payloads they refer to.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const rounds = 30;
let round = 0;
function step() {
    load("./resources/persistent-payload-lib.js");
    const kit = libOuter(round);
    assert(kit.inc() === round + 1 && kit.B.make()() === round + 1 && kit.arrow(1) === round + 2 && new LibCtor(4).f() === 4 && libSite() === libSite(), "the library works in round " + round);
    fullGC();
    const census = $vm.codeBlockCensus();
    // This file's, the library's as loaded in this round, and whatever of the last round the collector has not got to.
    assert(census.persistentPayloads <= 4, "payload slots do not pile up: " + census.persistentPayloads + " in round " + round);
    if (round++ >= rounds) {
        assert(globalThis.libLoads === rounds + 1, "loads " + globalThis.libLoads);
        return;
    }
    $vm.shrinkFootprintWhenIdle(true, false);
    setTimeout(step, 0);
}
step();
