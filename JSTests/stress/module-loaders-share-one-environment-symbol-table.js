//@ defaultRun
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=0")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useCodeRecoveryFromBytecodeCache=0")

// Records of several module loaders share one ModuleProgramExecutable, its function declarations' executables and with
// them their optimized code, while each record has its own environment. The optimizing tiers treat the scope of a symbol
// table that has only ever seen one environment as a constant, so everything that can run this code has to have an
// environment made from the executable's one symbol table: the second environment then invalidates the inference. That
// has to hold when the executable's code was deleted and generated again in between, and for a record that reads its
// function declarations long after it made its environment (useLazyModuleFunctionDeclarations).
// (stress/module-loaders-share-released-code.js has the cases where the code is deleted after a loader has finished and
// while one is suspended in the module's body.)

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const load = (path) => $vm.moduleLoaderImport($vm.createModuleLoader(), "./resources/module-loaders-one-symbol-table/" + path);
const tick = () => new Promise((resolve) => setTimeout(resolve, 1));
const n = 2 * testLoopCount;
async function deleteAllCode()
{
    $vm.deleteAllCodeWhenIdle();
    await tick();
    fullGC();
}
function warmUp(f)
{
    let last;
    for (let i = 0; i < n; ++i)
        last = f();
    return last;
}

// A record that has adopted the executable and made its environment, but has yet to run the module, has the code
// fetched again because all code was deleted while it waited for a dependency.
async function deletedWhileARecordWaits()
{
    globalThis.moduleLoadersOneSymbolTableGate = undefined;
    const a = await load("gated.js");
    assert(a.marker === 7 && a.importedId() === 1, "a ran");

    let openGate;
    globalThis.moduleLoadersOneSymbolTableGate = new Promise((resolve) => { openGate = resolve; });
    const started = globalThis.moduleLoadersOneSymbolTableStarted;
    const pending = load("gated.js");
    while (globalThis.moduleLoadersOneSymbolTableStarted === started)
        await tick();
    // gate.js is suspended in its body; gated.js is linked and waits for it.
    await deleteAllCode();
    openGate();
    const c = await pending;
    assert(c.marker === 7, "c ran");
    globalThis.moduleLoadersOneSymbolTableGate = undefined;

    const d = await load("gated.js");
    assert(d.marker === 7, "d ran");
    assert(warmUp(d.count) === 1 + n, "d.count");
    assert(warmUp(d.countThroughInner) === 1 + 2 * n, "d.countThroughInner");
    assert(warmUp(d.countFromExpression) === 1 + 3 * n, "d.countFromExpression");
    assert(warmUp(d.importedId) === 3 && warmUp(d.importedIdFromExpression) === 3 && warmUp(d.importedIdThroughNamespace) === 3, "d's import");

    for (const [name, ns, id] of [["a", a, 1], ["c", c, 2]]) {
        assert(warmUp(ns.count) === 1 + n, name + ".count counts its own calls");
        assert(warmUp(ns.countThroughInner) === 1 + 2 * n, name + ".countThroughInner counts its own calls");
        assert(warmUp(ns.countFromExpression) === 1 + 3 * n, name + ".countFromExpression counts its own calls");
        assert(warmUp(ns.importedId) === id, name + ".importedId reads its own loader's dependency");
        assert(warmUp(ns.importedIdFromExpression) === id, name + ".importedIdFromExpression reads its own loader's dependency");
        assert(warmUp(ns.importedIdThroughNamespace) === id, name + ".importedIdThroughNamespace reads its own loader's dependency");
    }
    assert(d.count() === 2 + 3 * n, "and left d's alone");
}

deletedWhileARecordWaits().then(() => { }, (error) => {
    print("FAIL", error, error && error.stack);
    $vm.abort();
});
