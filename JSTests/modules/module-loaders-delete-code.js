import { shouldBe } from "./resources/assert.js";

// After all code is deleted, a new loader links the module afresh rather than
// adopting the executable earlier loaders used; later loaders share the new one.
const load = () => $vm.moduleLoaderImport($vm.createModuleLoader(), "./module-loaders/main.js");

const a = await load();
shouldBe(a.run(1000), 1000);
$vm.deleteAllCodeWhenIdle();
await new Promise((resolve) => setTimeout(resolve, 0));
const b = await load();
shouldBe(JSON.stringify([b.run(10), a.run(1)]), `[10,1001]`);
shouldBe($vm.codeBlockFor(a.run) === $vm.codeBlockFor(b.run), false);
const c = await load();
shouldBe(typeof $vm.codeBlockFor(c.run), "string"); // never called, and already has b's code
shouldBe(c.run(20), 20);
