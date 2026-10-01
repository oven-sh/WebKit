import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

const loaders = [$vm.createModuleLoader(), $vm.createModuleLoader(), $vm.createModuleLoader()];
const cycles = [];
for (const loader of loaders)
    cycles.push([await $vm.moduleLoaderImport(loader, "./import-slots/cycle-a.js"), await $vm.moduleLoaderImport(loader, "./import-slots/cycle-b.js")]);
for (let k = 0; k < cycles.length; ++k) {
    const [a, b] = cycles[k];
    shouldBe([a.sawFromB, a.bValueSeen, a.viaB, b.earlyError].join(), "b:a(),bValue,a,ReferenceError: Cannot access 'aValue' before initialization.");
    a.setA(`set${k}`);
}
for (let i = 0; i < testLoopCount; ++i) {
    for (let k = 0; k < cycles.length; ++k)
        shouldBe([cycles[k][1].readA(), cycles[k][0].aValue].join(), `set${k},set${k}`);
}
shouldBe(sameCode(cycles[1][1].readA, cycles[0][1].readA), true);

const selves = [];
for (const loader of loaders)
    selves.push(await $vm.moduleLoaderImport(loader, "./import-slots/self.js"));
selves[1].bump();
selves[2].bump();
selves[2].bump();
shouldBe(JSON.stringify(selves.map(self => self.read())), "[[1,1,true],[2,2,true],[3,3,true]]");

const asyncs = [];
for (const loader of loaders)
    asyncs.push(await $vm.moduleLoaderImport(loader, "./import-slots/tla-main.js"));
for (const instance of asyncs) {
    shouldBe(JSON.stringify([instance.seenAtStart, instance.seenAfterAwait]), `[["ready","after"],["ready","after",1]]`);
    shouldBe(JSON.stringify(instance.read()), `["ready",1]`);
}
