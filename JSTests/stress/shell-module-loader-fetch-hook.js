//@ runDefault("--useDollarVM=1")

// setModuleLoaderFetchHook(fn): the shell calls fn(key) before it reads the file of a module. A string is the text of
// the module. What fn throws is thrown by the fetch hook of the loader. So a test can make modules without files,
// and can run script inside the fetch step of a load.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

function importThrough(loader, path) {
    const load = { state: "pending", value: undefined };
    $vm.moduleLoaderImport(loader, path).then(namespace => {
        load.value = namespace;
        load.state = "fulfilled";
    }, error => {
        load.value = error;
        load.state = "rejected";
    });
    drainMicrotasks();
    return load;
}

const made = "./shell-module-loader-fetch-hook/";
const files = "./resources/module-loader-load-sync/";
globalThis.loadSyncLog = [];
const keys = [];
const failure = new RangeError("from the hook");
let inner;
setModuleLoaderFetchHook(key => {
    keys.push(key.slice(key.lastIndexOf("/") + 1));
    if (key.endsWith("/made-root.js"))
        return `import { value } from "./made-dep.js"; export default value + 1;`;
    if (key.endsWith("/made-dep.js"))
        return `export const value = 40;`;
    if (key.endsWith("/throws.js"))
        throw failure;
    if (key.endsWith("/loads.js")) {
        // A load of its own, inside the fetch step of this one.
        inner = $vm.moduleLoaderLoadSync($vm.createModuleLoader(), made + "made-root.js").default;
        return `export default ${inner} + 1;`;
    }
});

// Modules that the hook makes. There are no such files.
let load = importThrough($vm.createModuleLoader(), made + "made-root.js");
shouldBe(load.state, "fulfilled", "a module that the hook makes");
shouldBe(load.value.default, 41, "the value of a module that the hook makes");
shouldBe(keys.join(), "made-root.js,made-dep.js", "the keys that the hook was asked for");

// Where the hook returns no string, the file is read.
load = importThrough($vm.createModuleLoader(), files + "dep.js");
shouldBe(load.state, "fulfilled", "a module of a file");
shouldBe(load.value.dep, 40, "the value of a module of a file");

// What the hook throws is what the load fails with.
load = importThrough($vm.createModuleLoader(), made + "throws.js");
shouldBe(load.state, "rejected", "a module whose fetch throws");
shouldBe(load.value, failure, "what the load of that module fails with");

// The hook runs script: here a synchronous load through another loader.
load = importThrough($vm.createModuleLoader(), made + "loads.js");
shouldBe(load.state, "fulfilled", "a module whose fetch loads another");
shouldBe(inner, 41, "the load inside the hook");
shouldBe(load.value.default, 42, "the value of a module whose fetch loads another");

// Without a function the shell reads files again.
setModuleLoaderFetchHook();
load = importThrough($vm.createModuleLoader(), made + "made-root.js");
shouldBe(load.state, "rejected", "a module that no hook makes");

let threw = false;
try {
    setModuleLoaderFetchHook(1);
} catch (error) {
    threw = error instanceof TypeError;
}
shouldBe(threw, true, "a hook that is not a function");
