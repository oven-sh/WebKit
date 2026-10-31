//@ runDefault("--useDollarVM=1", "--useJIT=0")

// A termination can begin inside CyclicModuleRecord::initializeEnvironment(), behind the step that sets the environment
// of the module. link() then puts the module back to "unlinked", and the environment has to go with that: the module
// is linked again by the next load, which sets an environment again (ASSERT(!m_moduleEnvironment) in an assertion build).
//
// The walk takes a termination at each trap check of an import() in turn ($vm.callWithTerminationAtTrapCheck()) and
// imports the module again. A cut inside the load is not the subject here: there the later import() can stay pending.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

const root = "./resources/module-loader-load-sync/root.js";
const terminationValue = "JavaScript execution terminated.";
globalThis.loadSyncLog = [];

function importRoot(loader) {
    const load = { state: "pending", value: undefined };
    $vm.moduleLoaderImport(loader, root).then(namespace => {
        load.value = namespace.value;
        load.state = "fulfilled";
    }, error => {
        load.value = error;
        load.state = "rejected";
    });
    drainMicrotasks();
    return load;
}

// The trap checks of a function are at fixed places once its inline caches are full.
for (let i = 0; i < 10; ++i)
    importRoot($vm.createModuleLoader());

let linkedAgain = 0;
for (let n = 1; ; ++n) {
    if (n > 100000)
        throw new Error("the import() has no end");
    const loader = $vm.createModuleLoader();
    const wasCut = $vm.callWithTerminationAtTrapCheck(() => { importRoot(loader); }, n);
    shouldBe($vm.hasPendingTermination(), false, `trap check ${n}: a termination is still pending`);

    loadSyncLog.length = 0;
    const later = importRoot(loader);
    if (later.state == "fulfilled") {
        shouldBe(later.value, 41, `trap check ${n}: the value of the later import()`);
        // The later import() linked the graph, and ran what the cut had not run.
        if (wasCut && loadSyncLog.length == 2)
            ++linkedAgain;
    } else if (later.state == "rejected") {
        // A module that the termination stopped in its evaluation keeps that as its error.
        if (later.value !== null && later.value !== terminationValue)
            throw new Error(`trap check ${n}: the later import() rejected with ${later.value}`);
    }
    if (!wasCut) {
        shouldBe(later.state, "fulfilled", `trap check ${n}: the later import() of a call that was not cut`);
        break;
    }
}
if (!linkedAgain)
    throw new Error("no later import() linked the graph again");
