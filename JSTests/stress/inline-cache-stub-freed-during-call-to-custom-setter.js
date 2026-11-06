//@ runDefault("--zeroExecutableMemoryOnFree=1", "--useConcurrentJIT=0", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=1000", "--useHandlerICInFTL=0")

// An inline cache stub that has called native code must stay until the call
// returns, also when the inline cache has dropped it in the meantime. The
// collector learns that the stub is running from the return address into it,
// which it finds on the stack (JITStubRoutineSet::mark). Here the only copy of
// that address is the one that the native callee saved, and a callee compiled
// with -mbranch-protection=pac-ret saves it signed: on a processor with pointer
// authentication it is no address in the stub until the signature is removed.
//
// The native callee is the setter of $vm's customAccessor, which is compiled
// that way on ARM64 Linux in every build. It stores to `result` of the value it
// is given, and that is a JS setter, which:
//   1. makes the inline cache drop the stub that is running, and
//   2. collects, which frees a dropped stub that the collector did not find.
// --zeroExecutableMemoryOnFree fills the freed stub with zeroes, so a return
// into it crashes every time instead of once in a while.
//
// Only the FTL's inline caches have code of their own for this store: below it
// they run a thunk that is never freed. There are two holders because the DFG
// calls the setter without an inline cache when every structure it has seen
// finds the setter on the same object.

function test(dropStub) {
    const holders = [$vm.createCustomTestGetterSetter(), $vm.createCustomTestGetterSetter()];
    const bases = holders.map(holder => Object.create(holder));

    // A function of its own for every test, so an inline cache of its own.
    const store = new Function("base", "value", "base.customAccessor = value; return $vm.ftlTrue();");
    noInline(store);

    let armed = false;
    let hookCalls = 0;
    const value = {
        set result(receiver) {
            hookCalls++;
            if (!armed)
                return;
            armed = false;
            dropStub(store, holders);
            fullGC();
        }
    };

    let ranInFTL = false;
    for (let i = 0; i < testLoopCount; i++)
        ranInFTL = store(bases[i & 1], value);
    if ($vm.useFTLJIT() && !ranInFTL)
        throw new Error("the store did not reach the FTL, so no stub that can be freed ran");

    armed = true;
    store(bases[0], value);

    if (armed)
        throw new Error("the setter did not run");
    if (hookCalls !== testLoopCount + 1)
        throw new Error(`expected ${testLoopCount + 1} calls of the setter, got ${hookCalls}`);
}

// The stub watches the property that it found the setter in.
test((store, holders) => {
    Object.defineProperty(holders[0], "customAccessor", { value: 1, writable: true, configurable: true });
});

// The inline cache replaces the stub by one that also knows the new structures.
test((store, holders) => {
    for (let i = 0; i < testLoopCount; i++) {
        const base = Object.create(holders[1]);
        base["property" + (i & 3)] = 1;
        store(base, { });
    }
});
