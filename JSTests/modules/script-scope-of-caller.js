// CallFrame::scopeOfClosestScript(): a host function finds the scope its calling script was made in, so the
// loader whose module that script is of, in every tier, whoever called the script and whatever was inlined.
import { shouldBe } from "./resources/assert.js";

const first = $vm.createModuleLoader({ owner: "A" });
const A = await $vm.moduleLoaderImport(first, "./script-scope-of-caller/code.js");
// The same code as A's: the two loaders' modules share executables.
const B = await $vm.moduleLoaderImport($vm.createModuleLoader({ owner: "B" }, first), "./script-scope-of-caller/code.js");
const G = await import("./script-scope-of-caller/code.js");
const all = [[A, "A"], [B, "B"], [G, undefined]];

function globalAsks() { const owner = $vm.ownerOfCaller(); return owner; }
function globalCalls(f) { const owner = f(); return owner; }

for (const [m, owner] of all)
    shouldBe(m.atTopLevel, owner);

for (let i = 0; i < testLoopCount; ++i) {
    for (const [m, owner] of all) {
        shouldBe(m.plain(), owner);
        shouldBe(m.arrow(), owner);
        shouldBe(m.nested(), owner);
        shouldBe(new m.Thing().owner, owner);
        shouldBe(new m.Thing().method(), owner);
        shouldBe(m.Thing.make(), owner);
        shouldBe([...m.generator()].join(), [owner, owner].join());
        shouldBe(m.throughBuiltin(), owner);
        shouldBe(m.hostFunctionCalledByBuiltin(), owner);
        shouldBe(m.throughHostFunction(), owner);
        shouldBe(m.bound()(), owner);
        shouldBe(m.directEval(), owner);
        // A script is its own loader's, whoever calls it.
        shouldBe(globalCalls(m.small), owner);
        shouldBe(A.calls(m.plain), owner);
        shouldBe(B.calls(m.small), owner);
        shouldBe(m.calls(A.small), "A");
        shouldBe(m.calls(globalAsks), undefined);
    }
}

for (const [m, owner] of all) {
    shouldBe(await m.afterAwait(), owner);
    shouldBe(await m.thenCallback(), owner);
}
