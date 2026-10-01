import { shouldBe } from "./resources/assert.js";

// Records for the same module in different loaders run the same linked code
// (CodeBlocks, JIT code) against their own environments.
const n = testLoopCount;
const loaders = [];
for (let i = 0; i < 6; ++i)
    loaders.push($vm.createModuleLoader());
const instance = (i) => $vm.moduleLoaderImport(loaders[i], "./module-loaders/main.js");

const a = await instance(0);
a.run(n);
const b = await instance(1);
shouldBe(typeof $vm.codeBlockFor(b.run), "string"); // never called, and already has a's code
b.run(n);
a.run(n);
shouldBe(JSON.stringify([a.read(), b.read()]), `[[${2 * n},${2 * n},true],[${n},${n},true]]`);

// Once a second instance exists the shared optimized code is instance-generic:
// further instances neither recompile it nor read another instance's bindings.
// (Compile counts are only deterministic without concurrent compilation.)
const compiles = numberOfDFGCompiles(a.run);
const reads = [];
for (let i = 2; i < 6; ++i) {
    const m = await instance(i);
    m.run(n + i);
    reads.push(m.read()[0]);
}
shouldBe(JSON.stringify(reads), `[${n + 2},${n + 3},${n + 4},${n + 5}]`);
if (!jscOptions().useConcurrentJIT)
    shouldBe(numberOfDFGCompiles(a.run), compiles);
shouldBe(JSON.stringify([a.read()[0], b.read()[0]]), `[${2 * n},${n}]`);
