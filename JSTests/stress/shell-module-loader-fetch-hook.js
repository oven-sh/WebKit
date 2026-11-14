//@ runDefault("--useDollarVM=1")

// setModuleLoaderFetchHook(fn): the shell calls fn(key) before it reads the file of a module. A string is the text of
// the module. An object makes a module without text, whose exports are the properties of the object. What fn throws is
// thrown by the fetch hook of the loader. So a test can make modules without files, and can run script inside the
// fetch step of a load.

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
const syncLoader = $vm.createModuleLoader();
setModuleLoaderFetchHook(key => {
    keys.push(key.slice(key.lastIndexOf("/") + 1));
    if (key.endsWith("/made-root.js"))
        return `import { value } from "./made-dep.js"; export default value + 1;`;
    if (key.endsWith("/made-dep.js"))
        return `export const value = 40;`;
    if (key.endsWith("/throws.js"))
        throw failure;
    if (key.endsWith("/imports-object.js"))
        return `import { answer, late } from "./object.js"; import { more } from "./more.js"; export default answer + late + more;`;
    if (key.endsWith("/more.js"))
        return `export const more = 0;`;
    if (key.endsWith("/sync-root.js"))
        return `import "./sync-begins-import.js"; import { value } from "./sync-dep.js"; export default value;`;
    if (key.endsWith("/sync-begins-import.js")) {
        // An import() of a module that the synchronous load has asked for already.
        $vm.moduleLoaderImport(syncLoader, made + "sync-dep.js").then(() => { }, () => { });
        return `export { };`;
    }
    if (key.endsWith("/sync-dep.js"))
        return `export const value = "made";`;
    if (key.endsWith("/object.js")) {
        return {
            answer: 41,
            get late() {
                keys.push("late is read");
                return 1;
            },
        };
    }
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

// Where the hook returns neither a string nor an object, the file is read.
load = importThrough($vm.createModuleLoader(), files + "dep.js");
shouldBe(load.state, "fulfilled", "a module of a file");
shouldBe(load.value.dep, 40, "the value of a module of a file");

// What the hook throws is what the load fails with.
load = importThrough($vm.createModuleLoader(), made + "throws.js");
shouldBe(load.state, "rejected", "a module whose fetch throws");
shouldBe(load.value, failure, "what the load of that module fails with");

// An object makes a module without text. The shell reads the exports when the graph is about to be linked, which is
// behind the fetch of each module of the graph.
keys.length = 0;
load = importThrough($vm.createModuleLoader(), made + "imports-object.js");
shouldBe(load.state, "fulfilled", "a module that imports a module without text");
shouldBe(load.value.default, 42, "the value of a module that imports a module without text");
shouldBe(keys.join(), "imports-object.js,object.js,more.js,late is read", "the exports of a module without text are read once, behind each fetch");
load = importThrough($vm.createModuleLoader(), made + "object.js");
shouldBe(load.state, "fulfilled", "a module without text");
shouldBe(load.value.answer, 41, "an export of a module without text");

// The hook runs script: here a synchronous load through another loader, where the build has that function.
if ($vm.moduleLoaderLoadSync) {
    load = importThrough($vm.createModuleLoader(), made + "loads.js");
    shouldBe(load.state, "fulfilled", "a module whose fetch loads another");
    shouldBe(inner, 41, "the load inside the hook");
    shouldBe(load.value.default, 42, "the value of a module whose fetch loads another");
}

// The hook is asked for each fetch of a JavaScript module, also where the loader passes the type of the module along:
// the import() inside this synchronous load is such a fetch.
if ($vm.moduleLoaderLoadSync) {
    keys.length = 0;
    shouldBe($vm.moduleLoaderLoadSync(syncLoader, made + "sync-root.js").default, "made", "a synchronous load with an import() inside its fetch step");
    shouldBe(keys.filter(key => key == "sync-dep.js").length >= 1, true, "the hook was asked for the module of the import()");
    drainMicrotasks();
}

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
