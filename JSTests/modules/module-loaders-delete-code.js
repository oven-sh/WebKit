import { shouldBe } from "./resources/assert.js";

// After all code is deleted, a new loader links the module afresh rather than
// adopting the executable earlier loaders used; later loaders share the new one.
const load = () => $vm.moduleLoaderImport($vm.createModuleLoader(), "./module-loaders/main.js");

const a = await load();
shouldBe(a.run(1000), 1000);
$vm.deleteAllCodeWhenIdle();
await new Promise((resolve) => setTimeout(resolve, 0));
const b = await load();
const c = await load();
shouldBe(JSON.stringify([b.run(10), c.run(20), a.run(1)]), `[10,20,1001]`);
shouldBe($vm.codeBlockFor(b.run) === $vm.codeBlockFor(c.run), true);
