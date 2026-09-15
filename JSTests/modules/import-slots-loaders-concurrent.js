import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

// Several loaders, the global object's own among them, start loading the same modules before any
// of them has linked or evaluated one: whichever record links a module first, the others are
// compared with it while its body has yet to run (tla-main.js and tla-dep.js suspend in theirs).
const loaders = [$vm.createModuleLoader(), $vm.createModuleLoader(), $vm.createModuleLoader()];
const started = [];
for (const specifier of ["./import-slots/tla-main.js", "./import-slots/loader-main.js"]) {
    started.push(import(specifier));
    for (const loader of loaders)
        started.push($vm.moduleLoaderImport(loader, specifier));
}
const instances = await Promise.all(started);
const asyncs = instances.slice(0, 4);
const mains = instances.slice(4);

for (const instance of asyncs) {
    shouldBe(JSON.stringify([instance.seenAtStart, instance.seenAfterAwait]), `[["ready","after"],["ready","after",1]]`);
    shouldBe(JSON.stringify(instance.read()), `["ready",1]`);
}
// (tla-main.js has incremented its loader's values.js once.)
for (let k = 0; k < mains.length; ++k)
    shouldBe(mains[k].run(k + 1), `${k + 2},${k + 1}`);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(mains.map(main => main.read()).join(";"), [1, 2, 3, 4].map(n => `${n + 1},${n + 1},${n},true,third:default`).join(";"));
for (let k = 1; k < 4; ++k) {
    shouldBe(sameCode(mains[0].run, mains[k].run), true);
    shouldBe(sameCode(asyncs[0].read, asyncs[k].read), true);
}
