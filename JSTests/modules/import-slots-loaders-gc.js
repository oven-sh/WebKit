import { shouldBe } from "./resources/assert.js";

// Instances come and go; the ones that stay keep reading their own bindings from code that
// instances since collected also ran.
const keep = [];
for (let round = 0; round < 12; ++round) {
    const instance = await $vm.moduleLoaderImport($vm.createModuleLoader(), "./import-slots/loader-main.js");
    shouldBe(instance.run(round + 1), `${round + 1},${round + 1}`);
    if (!(round % 4))
        keep.push([instance, round + 1]);
    if (round % 3 === 2)
        fullGC();
}
fullGC();
for (let i = 0; i < testLoopCount; ++i) {
    for (const [instance, n] of keep)
        shouldBe(instance.read(), `${n},${n},${n},true,third:default`);
}
const fresh = await $vm.moduleLoaderImport($vm.createModuleLoader(), "./import-slots/loader-main.js");
shouldBe(fresh.read(), "0,0,0,true,third:default");
shouldBe((await fresh.loadValues()) === fresh.values, true);
