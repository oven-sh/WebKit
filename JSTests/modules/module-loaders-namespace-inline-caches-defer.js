//@ requireOptions("--useImportDefer=1")
import { shouldBe } from "./resources/assert.js";

// A deferred namespace object exports the same names as the module's other namespace object, so the two have the same
// export layout. An inline cache that goes by that layout, filled by other loaders' namespace objects, must not read a
// deferred namespace object before its first [[Get]] has evaluated the module: a namespace object has no export slot
// for a name until a [[Get]] of that name on it has found the variable. "then" is never found on a deferred one.
const directory = "./module-loaders-namespace-inline-caches/";
const n = 2 * testLoopCount;

function readValue(object) { return object.value; }
noInline(readValue);
function readThen(object) { return object.then; }
noInline(readThen);
function readByVal(object, key) { return object[key]; }
noInline(readByVal);

// Namespace objects of three loaders, evaluated: the inline caches of the three functions go by layout.
const evaluated = [];
for (let k = 0; k < 3; ++k)
    evaluated.push(await $vm.moduleLoaderImport($vm.createModuleLoader(), directory + "deferred.js"));
const values = evaluated.map((namespace) => namespace.value);
for (let i = 0; i < n; ++i) {
    for (let k = 0; k < evaluated.length; ++k) {
        shouldBe(readValue(evaluated[k]), values[k]);
        shouldBe(readThen(evaluated[k]), "then");
        shouldBe(readByVal(evaluated[k], "value"), values[k]);
        shouldBe(readByVal(evaluated[k], "then"), "then");
    }
}

// A deferred namespace object of a fourth loader. Reading "then" does not evaluate the module, and finds nothing.
const evaluations = globalThis.moduleLoadersNamespaceInlineCachesEvaluations;
const defers = await $vm.moduleLoaderImport($vm.createModuleLoader(), directory + "defers.js");
const deferred = defers.deferredNamespace();
for (let i = 0; i < n; ++i) {
    shouldBe(readThen(deferred), undefined);
    shouldBe(readByVal(deferred, "then"), undefined);
}
shouldBe(globalThis.moduleLoadersNamespaceInlineCachesEvaluations, evaluations);

// The first read of an exported name evaluates it, once.
for (let i = 0; i < n; ++i) {
    shouldBe(readValue(deferred), "value" + (evaluations + 1));
    shouldBe(readByVal(deferred, "value"), "value" + (evaluations + 1));
    shouldBe(globalThis.moduleLoadersNamespaceInlineCachesEvaluations, evaluations + 1);
    shouldBe(readThen(deferred), undefined);
    shouldBe(readValue(evaluated[i % 3]), values[i % 3]);
}

// A deferred module that throws: every read throws, and it is evaluated once.
const throwing = defers.throwingNamespace();
let thrown = 0;
for (let i = 0; i < n; ++i) {
    try {
        readValue(throwing);
    } catch (error) {
        if (error.message === "thrown by the deferred module")
            ++thrown;
    }
    shouldBe(readThen(throwing), undefined);
}
shouldBe(thrown, n);
shouldBe(globalThis.moduleLoadersNamespaceInlineCachesThrows, 1);
