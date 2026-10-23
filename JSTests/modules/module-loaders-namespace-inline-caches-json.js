import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

// The namespace object of a record that is not a source text module (here a JSON module), read by code that loaders
// share. Each loader parses its own value. The export layout of such a namespace object is like any other: an inline
// cache entry that goes by layout reads each loader's own binding through that loader's namespace object.
const directory = "./module-loaders-namespace-inline-caches/";
const n = 2 * testLoopCount;
const goesByLayout = !!jscOptions().useModuleNamespaceLoadByExportLayout;
const isJITCompiled = (f) => /Baseline|DFG|FTL/.test($vm.codeBlockFor(f));

const readers = [];
for (let k = 0; k < 3; ++k)
    readers.push(await $vm.moduleLoaderImport($vm.createModuleLoader(), directory + "json-reader.js"));
shouldBe(readers[0].readDefault() !== readers[1].readDefault(), true);
shouldBe(sameCode(readers[0].readDefault, readers[1].readDefault), true);
readers[1].readDefault().value = 5;
for (let i = 0; i < n; ++i) {
    for (let k = 0; k < readers.length; ++k)
        shouldBe(readers[k].readDefault().value, k == 1 ? 5 : 1);
}
const wasJITCompiled = isJITCompiled(readers[0].readDefault);
for (let i = 0; i < 100; ++i) {
    for (let k = 0; k < readers.length; ++k)
        shouldBe(readers[k].readDefault().value, k == 1 ? 5 : 1);
}
if (wasJITCompiled || !goesByLayout)
    shouldBe($vm.haveSameExportLayout(readers[0].namespace(), readers[2].namespace()), goesByLayout);
