import { shouldBe } from "./resources/assert.js";

// A module whose text is unchanged but whose dependency changed shape between two
// loaders fetching it links its own code in the second loader instead of running
// the first loader's; a third loader then shares the second's. The modules are
// written by the test under names of its own, so concurrent runs do not interfere.
const id = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
const importer = `./module-loaders-changed-dependency/${id}-importer.js`;
const dep = `./module-loaders-changed-dependency/${id}-dep.js`;
const version1 = `export let x = "x1";\nexport const shape = 1;\n`;
const version2 = `export let w = "w";\nexport let x = "x2";\nexport const shape = 2;\n`;
writeFile(importer, `import { x, shape } from "./${id}-dep.js"\nexport function read(n) { let result; for (let i = 0; i < n; ++i) result = x; return [result, shape]; }\n`);
const load = () => $vm.moduleLoaderImport($vm.createModuleLoader(), importer);

writeFile(dep, version1);
const a = await load();
const readA = a.read(testLoopCount);
writeFile(dep, version2);
const b = await load();
const readB = b.read(testLoopCount);
const c = await load();
shouldBe(typeof $vm.codeBlockFor(c.read), "string"); // never called, and already has b's code
shouldBe(JSON.stringify([readA, a.read(1), readB, c.read(testLoopCount)]), `[["x1",1],["x1",1],["x2",2],["x2",2]]`);
shouldBe($vm.codeBlockFor(a.read) === $vm.codeBlockFor(b.read), false);
