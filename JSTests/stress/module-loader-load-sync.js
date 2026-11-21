//@ runDefault("--useDollarVM=1")

// $vm.moduleLoaderLoadSync({ loader }, specifier): JSModuleLoader::loadModuleSync(), the load that `require()` of an
// ES module makes in Bun. The graph is loaded, linked and evaluated when the call returns.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

function shouldThrow(fn, constructor, message, what) {
    try {
        fn();
    } catch (error) {
        if (error instanceof constructor && error.message == message)
            return;
        throw new Error(`${what}: threw ${error}`);
    }
    throw new Error(`${what}: did not throw`);
}

const directory = "./resources/module-loader-load-sync/";
globalThis.loadSyncLog = [];

// The bodies have run, in the order of the graph, when the call returns.
const loader = $vm.createModuleLoader();
const namespace = $vm.moduleLoaderLoadSync(loader, directory + "root.js");
shouldBe(loadSyncLog.join(), "dep,root", "the bodies that ran before the call returned");
shouldBe(namespace.value, 41, "the namespace object");

// A second load, synchronous or not, answers with the module that is there.
shouldBe($vm.moduleLoaderLoadSync(loader, directory + "root.js"), namespace, "the second synchronous load");
shouldBe($vm.moduleLoaderLoadSync(loader, directory + "dep.js").dep, 40, "a synchronous load of the dependency");
let imported;
$vm.moduleLoaderImport(loader, directory + "root.js").then(result => { imported = result; });
drainMicrotasks();
shouldBe(imported, namespace, "import() after the synchronous load");
shouldBe(loadSyncLog.join(), "dep,root", "the bodies ran once");

// A module that import() has begun to load is taken over: the call does not wait for the jobs of that load.
{
    const loader = $vm.createModuleLoader();
    let imported;
    $vm.moduleLoaderImport(loader, directory + "root.js").then(result => { imported = result; });
    const namespace = $vm.moduleLoaderLoadSync(loader, directory + "root.js");
    shouldBe(namespace.value, 41, "a synchronous load of a module that import() is loading");
    shouldBe(imported, undefined, "the import() has not been answered yet");
    drainMicrotasks();
    shouldBe(imported, namespace, "the import() that was first");
    shouldBe(loadSyncLog.join(), "dep,root,dep,root", "the bodies ran once for the second loader");
}

// What the module throws is thrown by the call.
shouldThrow(() => $vm.moduleLoaderLoadSync($vm.createModuleLoader(), directory + "throws.js"), RangeError, "from the module", "a module that throws");

// A module with a top-level await does not finish inside the call.
shouldThrow(() => $vm.moduleLoaderLoadSync($vm.createModuleLoader(), directory + "waits.js"), TypeError, "the load of the module did not finish", "a module that waits");

shouldThrow(() => $vm.moduleLoaderLoadSync({ }, directory + "root.js"), TypeError, "expected the result of $vm.createModuleLoader()", "not a loader");
