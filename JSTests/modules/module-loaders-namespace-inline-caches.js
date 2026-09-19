import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

// Records for one module in different loaders share the module's code, and with it the code's inline caches, while
// each record's code reads the namespace objects of its own loader. An inline cache entry for one namespace object
// serves one loader only. So from the second namespace object an inline cache sees, its entries go by export layout
// (ModuleNamespaceExportLayout): one entry serves every namespace object that exports the same names, and reads the
// variable through that object's export slot. Every loader has to keep reading its own bindings, live, in every tier.
const directory = "./module-loaders-namespace-inline-caches/";
const n = 2 * testLoopCount;
const goesByLayout = !!jscOptions().useModuleNamespaceLoadByExportLayout;
const isJITCompiled = (f) => /Baseline|DFG|FTL/.test($vm.codeBlockFor(f));

async function makeInstance()
{
    const loader = $vm.createModuleLoader();
    const reader = await $vm.moduleLoaderImport(loader, directory + "reader.js");
    const dep = await $vm.moduleLoaderImport(loader, directory + "dep.js");
    return { reader, dep, id: dep.id, bumps: 0 };
}

function check(instance)
{
    const { reader, id } = instance;
    const current = id + instance.bumps;
    shouldBe(reader.readId(), current);
    shouldBe(reader.readTag(), "dep" + id);
    shouldBe(reader.readDefault(), "default" + id);
    shouldBe(reader.readDeclared()(), "declared" + current);
    shouldBe(reader.readMissing(), undefined);
    shouldBe(reader.readByVal("id"), current);
    shouldBe(reader.readByVal("tag"), "dep" + id);
    shouldBe(reader.readThroughBarrel(), current);
    shouldBe(reader.readRenamed(), current);
    shouldBe(reader.readWhole(), current);
    shouldBe(reader.sum(10), 20 * current);
}

// Code that loaders share reads the namespace objects of the loader it runs for.
{
    // One loader alone: its inline cache entries are for its one namespace object, and the DFG folds them.
    const first = await makeInstance();
    for (let i = 0; i < n; ++i)
        check(first);

    // More loaders, each run hot on its own first, then all of them in turn.
    const instances = [first];
    for (let k = 0; k < 3; ++k) {
        const instance = await makeInstance();
        shouldBe(instance.id !== first.id, true);
        shouldBe(sameCode(instance.reader.readId, first.reader.readId), true);
        instances.push(instance);
        for (let i = 0; i < n; ++i)
            check(instance);
    }
    for (let i = 0; i < n; ++i)
        instances.forEach(check);

    // Inline caches that have seen the namespace objects of several loaders go by export layout, which namespace
    // objects with the same exported names share. (The interpreter has no inline cache for these loads.)
    const wasJITCompiled = isJITCompiled(first.reader.readId);
    for (let i = 0; i < 100; ++i)
        instances.forEach(check);
    if (wasJITCompiled || !goesByLayout) {
        shouldBe($vm.haveSameExportLayout(instances[0].dep, instances[3].dep), goesByLayout);
        shouldBe($vm.haveSameExportLayout(instances[1].dep, instances[2].dep), goesByLayout);
    }

    // The bindings are live, and each loader's own.
    for (let i = 0; i < 100; ++i) {
        const instance = instances[i % 3];
        shouldBe(instance.reader.bump(), instance.id + ++instance.bumps);
        instances.forEach(check);
    }

    // A long loop, entered through OSR.
    for (const instance of instances)
        shouldBe(instance.reader.sum(100 * n), 200 * n * (instance.id + instance.bumps));

    // Loaders come and go. A layout stays while a namespace object has it, and is made again otherwise.
    let last = instances[3];
    instances.length = 0;
    for (let round = 0; round < 3; ++round) {
        fullGC();
        const instance = await makeInstance();
        for (let i = 0; i < n; ++i) {
            check(instance);
            check(last);
        }
        last = instance;
        if (round == 1) {
            last = null;
            fullGC();
            last = await makeInstance();
        }
    }
}

// One function, not shared, that is given namespace objects of several loaders and modules.
{
    function readId(namespace) { return namespace.id; }
    noInline(readId);
    function readTag(namespace) { return namespace.tag; }
    noInline(readTag);

    const loaders = [$vm.createModuleLoader(), $vm.createModuleLoader(), $vm.createModuleLoader()];
    const deps = [];
    for (const loader of loaders)
        deps.push(await $vm.moduleLoaderImport(loader, directory + "dep.js"));
    const ids = deps.map((dep) => dep.id);
    // The same exported names as dep.js, so the same layout, in another module.
    const twin = await $vm.moduleLoaderImport(loaders[0], directory + "twin.js");
    // "id" is at another position in this one, so it has another layout.
    const other = await $vm.moduleLoaderImport(loaders[0], directory + "other.js");
    const plain = { id: "plain", tag: "plain tag" };

    for (let i = 0; i < n; ++i)
        shouldBe(readId(deps[0]), ids[0]);
    for (let i = 0; i < n; ++i) {
        for (let k = 0; k < deps.length; ++k) {
            shouldBe(readId(deps[k]), ids[k]);
            shouldBe(readTag(deps[k]), "dep" + ids[k]);
        }
        shouldBe(readId(twin), -1);
        shouldBe(readTag(twin), "twin");
        shouldBe(readId(other), 1000);
        shouldBe(readTag(other), undefined);
        shouldBe(readId(plain), "plain");
        shouldBe(readTag(plain), "plain tag");
    }
    if (isJITCompiled(readId) || !goesByLayout) {
        shouldBe($vm.haveSameExportLayout(deps[0], twin), goesByLayout);
        shouldBe($vm.haveSameExportLayout(deps[0], other), false);
    }
    twin.bump();
    deps[1].bump();
    for (let i = 0; i < n; ++i) {
        shouldBe(readId(twin), -2);
        shouldBe(readId(deps[1]), ids[1] + 1);
        shouldBe(readId(deps[0]), ids[0]);
    }
}

// A binding that is still uninitialized for one loader, read by code whose inline cache other loaders have filled.
{
    const entries = [];
    for (let k = 0; k < 4; ++k) {
        const entry = await $vm.moduleLoaderImport($vm.createModuleLoader(), directory + "tdz-entry.js");
        shouldBe(entry.early, "ReferenceError");
        entries.push(entry);
        for (let i = 0; i < n; ++i) {
            for (const each of entries)
                shouldBe(typeof each.readLate(), "number");
        }
    }
}
