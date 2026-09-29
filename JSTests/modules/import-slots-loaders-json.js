import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

// An import from a module that is not a source text module: what the importer's code embeds is the
// binding's offset in that module's environment, and records whose offsets agree share the code.
// Each loader has its own JSON module, so its own object.
const load = () => $vm.moduleLoaderImport($vm.createModuleLoader(), "./import-slots/json-user.js");
const a = await load();
const b = await load();
const c = await load();
b.increment();
b.data.n = 5;
for (let i = 0; i < testLoopCount; ++i)
    shouldBe([a.read(), b.read(), c.read()].join(";"), "json,1,0,true;json,5,1,true;json,1,0,true");
shouldBe(sameCode(a.read, b.read), true);
shouldBe(sameCode(a.read, c.read), true);
shouldBe(a.data === b.data, false);
shouldBe(a.data === c.data, false);

const own = await import("./import-slots/json-user.js");
shouldBe(own.read(), "json,1,0,true");
shouldBe(sameCode(own.read, a.read), true);
shouldBe(own.data === a.data, false);
