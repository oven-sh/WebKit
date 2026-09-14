import { shouldBe } from "./resources/assert.js";
import * as own from "./module-loaders/main.js"

// A module loader created with $vm has its own registry: importing the same
// files through it links and evaluates them again, with their own state.
const a = await $vm.moduleLoaderImport($vm.createModuleLoader(), "./module-loaders/main.js");
const b = await $vm.moduleLoaderImport($vm.createModuleLoader(), "./module-loaders/main.js");

shouldBe(a === own || b === own || a === b, false);
shouldBe(JSON.stringify([own.run(1), a.run(2), b.run(3)]), `[1,2,3]`);
shouldBe(JSON.stringify([own.read(), a.read(), b.read()]), `[[1,1,true],[2,2,true],[3,3,true]]`);
shouldBe(a.counter === b.counter, false);

// import() inside a loader's module resolves in that loader.
shouldBe((await a.loadCounter()) === a.counter, true);
shouldBe((await b.loadCounter()) === b.counter, true);
shouldBe((await own.loadCounter()) === own.counter, true);
const [lazyA, lazyB] = [await a.loadLazy(), await b.loadLazy()];
shouldBe(lazyA === lazyB, false);
shouldBe(JSON.stringify([lazyA.hit(), lazyA.hit(), lazyB.hit()]), `[1,2,1]`);

// Import cycles link and evaluate per loader, including module code that runs
// before its own module is evaluated.
const cycleA = await $vm.moduleLoaderImport($vm.createModuleLoader(), "./module-loaders/cycle-a.js");
const cycleB = await $vm.moduleLoaderImport($vm.createModuleLoader(), "./module-loaders/cycle-a.js");
shouldBe(JSON.stringify([cycleA.fromB, cycleA.early, cycleB.fromB, cycleB.early]), `["bac","ac","bac","ac"]`);
