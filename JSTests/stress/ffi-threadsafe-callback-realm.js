//@ requireOptions("--useDollarVM=1", "--useExecutableAllocationFuzz=false")
// A call that a threadsafe callback accepted is made, though nothing refers to the callback's global object any more. When
// the calls are made, the global object can be collected.
if (!$vm.useJIT()) quit();

const signature = { args: ["i32"], returns: "void" };
const realmCount = Math.max(2, Math.min(20, testLoopCount / 100 | 0));
const callsPerRealm = 3;

let sum = 0;
let collected = 0;
const registry = new FinalizationRegistry(() => { ++collected; });
function inNewRealm(close) {
    const realm = createGlobalObject();
    registry.register(realm, undefined);
    const callback = realm.$vm.ffiCallback(signature, realm.Function("add", "return value => add(value)")(value => { sum += value; }), { threadsafe: true });
    const call = $vm.ffiFunction(signature, callback.ptr, "call_threadsafe");
    for (let i = 1; i <= callsPerRealm; ++i)
        call(i);
    if (close)
        callback.close();
}
noInline(inNewRealm);

function clobberStack(depth) {
    const array = [depth, depth, depth, depth, depth, depth];
    return depth ? clobberStack(depth - 1) + array.length : 0;
}
noInline(clobberStack);
function collect() {
    clobberStack(200);
    for (let i = 0; i < 4; ++i)
        fullGC();
}

for (const close of [true, false]) {
    sum = 0;
    for (let i = 0; i < realmCount; ++i)
        inNewRealm(close);
    collect();

    const drained = $vm.drainThreadsafeCallbacks();
    if (drained !== realmCount * callsPerRealm)
        throw new Error("expected " + realmCount * callsPerRealm + " invocations, drained " + drained);
    if (sum !== realmCount * (1 + 2 + 3))
        throw new Error("not every invocation ran: " + sum);
    if ($vm.drainThreadsafeCallbacks() !== 0)
        throw new Error("queue not empty");
    collect();
}

// The jobs of the registry run once this script has.
asyncTestStart(1);
setTimeout(() => {
    collect();
    setTimeout(() => {
        if (collected < realmCount)
            throw new Error("the global objects were kept: " + collected + " collected");
        asyncTestPassed();
    });
});
