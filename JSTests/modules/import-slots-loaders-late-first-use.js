import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

// One instance makes the code hot without ever reading some of its imports; another instance,
// running that same code, is the first to read them.
const load = () => $vm.moduleLoaderImport($vm.createModuleLoader(), "./import-slots/late.js");
const a = await load();
shouldBe(a.loop(testLoopCount, false), 0);
const b = await load();
const c = await load();
shouldBe(typeof $vm.codeBlockFor(b.sometimes), "string");
shouldBe(sameCode(a.sometimes, b.sometimes), true);
b.bump();
c.bump();
c.bump();
c.increment();
shouldBe([b.sometimes(true), c.sometimes(true), a.sometimes(true)].join(";"), "42,1,third:third;42,2,third:third;42,0,third:third");
shouldBe([b.loop(testLoopCount, true), c.loop(testLoopCount, true), a.loop(testLoopCount, true)].join(";"), "42,1,third:third;42,2,third:third;42,0,third:third");
shouldBe([a.sometimes(false), b.sometimes(false), c.sometimes(false)].join(), "0,0,1");
