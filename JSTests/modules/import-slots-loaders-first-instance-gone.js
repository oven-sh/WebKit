import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

// The first instance of a module is what the second is compared with before it runs its code.
// If the first has been collected before there is a second, there is nothing to compare with:
// the next instance links its own code, and the ones after that share with it. (Whether or not
// the collector has got to the first instance by then, the instances below behave the same.)
const load = () => $vm.moduleLoaderImport($vm.createModuleLoader(), "./import-slots/loader-main.js");
await (async () => {
    const first = await load();
    shouldBe(first.run(testLoopCount), `${testLoopCount},${testLoopCount}`);
})();
for (let i = 0; i < 4; ++i) {
    await Promise.resolve();
    fullGC();
}
const a = await load();
shouldBe(a.run(1), "1,1");
shouldBe(a.read(), "1,1,1,true,third:default");
const b = await load();
const c = await load();
shouldBe(typeof $vm.codeBlockFor(b.run), "string");
shouldBe(sameCode(a.run, b.run), true);
shouldBe(sameCode(a.read, c.read), true);
shouldBe(b.run(2), "2,2");
for (let i = 0; i < testLoopCount; ++i)
    shouldBe([a.read(), b.read(), c.read()].join(";"), "1,1,1,true,third:default;2,2,2,true,third:default;0,0,0,true,third:default");
