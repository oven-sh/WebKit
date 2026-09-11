import { shouldBe } from "./resources/assert.js";

// A module whose text is unchanged but whose dependency changed shape between two
// loaders fetching it links its own code in the second loader instead of running
// the first loader's; a third loader then shares the second's.
const version1 = `export let x = "x1";\nexport const shape = 1;\n`;
const version2 = `export let w = "w";\nexport let x = "x2";\nexport const shape = 2;\n`;
const dep = "./module-loaders-changed-dependency/dep.js";
const load = () => $vm.moduleLoaderImport($vm.createModuleLoader(), "./module-loaders-changed-dependency/importer.js");

writeFile(dep, version1);
const a = await load();
const readA = a.read(100000);
writeFile(dep, version2);
try {
    const b = await load();
    const c = await load();
    shouldBe(JSON.stringify([readA, a.read(1), b.read(100000), c.read(100000)]), `[["x1",1],["x1",1],["x2",2],["x2",2]]`);
    shouldBe($vm.codeBlockFor(a.read) === $vm.codeBlockFor(b.read), false);
    shouldBe($vm.codeBlockFor(b.read) === $vm.codeBlockFor(c.read), true);
} finally {
    writeFile(dep, version1);
}
