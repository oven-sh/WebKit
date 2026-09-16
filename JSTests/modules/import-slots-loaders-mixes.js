import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

// Records that share code number their import slots alike, whatever mix of namespace and
// named imports the module has, and keep their own bindings in every tier.
const loaders = [];
for (let i = 0; i < 4; ++i)
    loaders.push($vm.createModuleLoader());
const expectations = {
    "mix-first.js": n => `${n},${n},second:first,true`,
    "mix-middle.js": n => `${n},${n},second:first,second:first,true`,
    "mix-last.js": n => `${n},${n},second:first,third:third,true,true`,
    "only-namespaces.js": n => `${n},second:first`,
    "no-imports.js": n => `${n}`,
    "aliases.js": n => `${n},${n},${n},${n},true`,
};
for (const [file, expected] of Object.entries(expectations)) {
    const instances = [];
    for (const loader of loaders)
        instances.push(await $vm.moduleLoaderImport(loader, `./import-slots/${file}`));
    // values.js is one instance per loader, shared by the files above: read its count first.
    const base = instances.map(instance => Number(instance.read().split(",")[0]));
    for (let i = 0; i < testLoopCount; ++i)
        instances[0].read();
    for (let k = 1; k < instances.length; ++k) {
        shouldBe(sameCode(instances[k].read, instances[0].read), true);
    }
    for (let k = 0; k < instances.length; ++k) {
        for (let i = 0; i <= k; ++i)
            instances[k].increment();
    }
    for (let round = 0; round < testLoopCount; ++round) {
        for (let k = 0; k < instances.length; ++k)
            shouldBe(instances[k].read(), expected(base[k] + k + 1));
    }
}
