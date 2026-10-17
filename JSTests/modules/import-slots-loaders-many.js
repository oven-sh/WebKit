import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

const load = () => $vm.moduleLoaderImport($vm.createModuleLoader(), "./import-slots/many.js");
const [a, b, c] = [await load(), await load(), await load()];
let base = 0;
for (let m = 0; m < 8; ++m) {
    for (let i = 0; i < 16; ++i)
        base += m * 100 + i;
}
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(a.sum(), base);
shouldBe(typeof $vm.codeBlockFor(b.sum), "string");
shouldBe(sameCode(a.sum, b.sum), true);
b.bumpEverything();
c.bumpEverything();
c.bumpEverything();
for (let i = 0; i < testLoopCount; ++i)
    shouldBe([a.sum(), b.sum(), c.sum(), a.last(), b.last(), c.last(), a.first(), b.first(), c.first()].join(), [base, base + 128, base + 256, 715, 716, 717, 0, 1, 2].join());
