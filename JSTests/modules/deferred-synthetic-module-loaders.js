import { shouldBe } from "./resources/assert.js";

// Records for the same module in different loaders share linked code, which embeds where each imported binding is in
// the environment of the record that exports it. A *.deferred.js record (SyntheticSourceProvider::createDeferred())
// has no environment when its importer links: it puts the bindings where the shared code reads them.
const n = testLoopCount;
const loaders = [];
for (let i = 0; i < 4; ++i)
    loaders.push($vm.createModuleLoader());
const instance = (i) => $vm.moduleLoaderImport(loaders[i], "./deferred-synthetic-module-loaders/main.js");

const a = await instance(0);
shouldBe(globalThis.loadersLog.join(), "first:undefined,lib1,main:x1");
shouldBe(a.run(n), 3 * n);

const b = await instance(1);
shouldBe(globalThis.loadersLog.join(), "first:undefined,lib1,main:x1,first:undefined,lib2,main:x2");
shouldBe(typeof $vm.codeBlockFor(b.run), "string"); // never called, and already has a's code
shouldBe(b.run(n), 4 * n);
shouldBe(a.run(n), 3 * n);
shouldBe(JSON.stringify([a.read(), b.read(), a.keys(), b.keys()]), JSON.stringify(["x1", "x2", "x,last", "padding1,x,last"]));

for (let i = 2; i < 4; ++i) {
    const m = await instance(i);
    shouldBe(m.read(), "x" + (i + 1));
    shouldBe(m.run(n), (3 + i) * n);
}
shouldBe(JSON.stringify([a.read(), b.read()]), JSON.stringify(["x1", "x2"]));
