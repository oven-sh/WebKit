import { shouldBe } from "./resources/assert.js";

// A module whose imports do not resolve fails to link in every loader, each time it is asked for,
// with the same error; the modules it would have shared with others stay usable, one instance
// per loader.
const loaders = [$vm.createModuleLoader(), $vm.createModuleLoader(), $vm.createModuleLoader()];
async function rejection(loader, specifier) {
    try {
        await $vm.moduleLoaderImport(loader, specifier);
    } catch (error) {
        return [error instanceof SyntaxError, String(error)].join();
    }
    return "loaded";
}
for (const specifier of ["./import-slots/imports-missing.js", "./import-slots/imports-conflict.js"]) {
    const expected = await rejection(loaders[0], specifier);
    shouldBe(expected.startsWith("true,SyntaxError: "), true);
    for (let round = 0; round < 2; ++round) {
        for (const loader of loaders)
            shouldBe(await rejection(loader, specifier), expected);
    }
}
const values = [];
for (const loader of loaders)
    values.push(await $vm.moduleLoaderImport(loader, "./import-slots/values.js"));
values[1].increment();
shouldBe(values.map(instance => instance.count).join(), "0,1,0");
