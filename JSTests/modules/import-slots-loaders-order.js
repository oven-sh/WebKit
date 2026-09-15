import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

const specifier = "./import-slots/loader-main.js";
const load = (loader, what = specifier) => $vm.moduleLoaderImport(loader, what);

// Loaded by the global object's own loader while it is the only loader there is: nothing about
// its imports was resolved for sharing then.
const own = await import(specifier);
shouldBe(own.run(testLoopCount), `${testLoopCount},${testLoopCount}`);

// Loaders made afterwards have their own instances, and run that same code: the comparison
// happens when the second record for the module turns up, whichever loader it is in.
const a = await load($vm.createModuleLoader());
const b = await load($vm.createModuleLoader());
const c = await load($vm.createModuleLoader());
shouldBe(typeof $vm.codeBlockFor(a.run), "string");
shouldBe(a.run(1), "1,1");
shouldBe(a.read(), "1,1,1,true,third:default");
shouldBe(typeof $vm.codeBlockFor(c.read), "string");
shouldBe(sameCode(a.run, own.run), true);
shouldBe(sameCode(a.run, b.run), true);
shouldBe(sameCode(a.run, c.run), true);
shouldBe(b.run(2), "2,2");
shouldBe(c.run(3), "3,3");
shouldBe([own.read(), a.read(), b.read(), c.read()].join(";"), [testLoopCount, 1, 2, 3].map(n => `${n},${n},${n},true,third:default`).join(";"));

// The same for a module the global object's own loader first loads after other loaders exist,
// in either order.
const ownLate = await import("./import-slots/late.js");
shouldBe(ownLate.loop(testLoopCount, false), testLoopCount);
const lateA = await load($vm.createModuleLoader(), "./import-slots/late.js");
shouldBe(typeof $vm.codeBlockFor(lateA.sometimes), "string");
shouldBe(sameCode(ownLate.sometimes, lateA.sometimes), true);
lateA.increment();
shouldBe([ownLate.sometimes(false), lateA.sometimes(false), lateA.sometimes(true), ownLate.sometimes(true)].join(";"), `${testLoopCount};1;42,0,third:third;42,${testLoopCount},third:third`);

const aliasesA = await load($vm.createModuleLoader(), "./import-slots/aliases.js");
shouldBe(aliasesA.read(), "0,0,0,0,true");
const ownAliases = await import("./import-slots/aliases.js");
shouldBe(typeof $vm.codeBlockFor(ownAliases.read), "string");
shouldBe(sameCode(ownAliases.read, aliasesA.read), true);
aliasesA.increment();
shouldBe([ownAliases.read(), aliasesA.read()].join(";"), `${testLoopCount},${testLoopCount},${testLoopCount},${testLoopCount},true;1,1,1,1,true`);
