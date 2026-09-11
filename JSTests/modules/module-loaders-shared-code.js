import { shouldBe } from "./resources/assert.js";

// Records for the same module in different loaders run the same linked code
// (CodeBlocks, JIT code) against their own environments.
const loaders = [];
for (let i = 0; i < 6; ++i)
    loaders.push($vm.createModuleLoader());
const instance = (i) => $vm.moduleLoaderImport(loaders[i], "./module-loaders/main.js");

const a = await instance(0);
a.run(200000);
const b = await instance(1);
shouldBe($vm.codeBlockFor(a.run) === $vm.codeBlockFor(b.run), true);
b.run(200000);
a.run(200000);
shouldBe(JSON.stringify([a.read(), b.read()]), `[[400000,400000,true],[200000,200000,true]]`);

// Once a second instance exists the shared optimized code is instance-generic:
// further instances neither recompile it nor read another instance's bindings.
const compiles = numberOfDFGCompiles(a.run);
const reads = [];
for (let i = 2; i < 6; ++i) {
    const m = await instance(i);
    m.run(200000 + i);
    reads.push(m.read()[0]);
}
shouldBe(JSON.stringify(reads), `[200002,200003,200004,200005]`);
shouldBe(numberOfDFGCompiles(a.run), compiles);
shouldBe(JSON.stringify([a.read()[0], b.read()[0]]), `[400000,200000]`);
