import { shouldBe } from "./resources/assert.js";
import { sameCode } from "./import-slots/code.js";

// The first loader's gate-main.js links while it is the only record of the module, and then waits
// for its dependency, which is suspended at a top-level await. Meanwhile a second loader's record
// takes the first one's code, evaluates and makes that code hot. The first record evaluates only
// after that; its dependency's body, shared by then too, resumes in the middle.
let release;
const gates = [new Promise(resolve => { release = resolve; }), Promise.resolve()];
globalThis.importSlotsGate = { started: 0, next: () => gates.shift() };
const specifier = "./import-slots/gate-main.js";

const pendingFirst = $vm.moduleLoaderImport($vm.createModuleLoader(), specifier);
while (globalThis.importSlotsGate.started < 1)
    await Promise.resolve();

const second = await $vm.moduleLoaderImport($vm.createModuleLoader(), specifier);
shouldBe(second.seenAtStart, "ready,0,1,1,0");
second.increment();
second.bump();
shouldBe(second.loop(testLoopCount), "ready,2,1");

release();
const first = await pendingFirst;
shouldBe(first.seenAtStart, "ready,0,1,1,0");
shouldBe(sameCode(first.read, second.read), true);
shouldBe(first.loop(testLoopCount), "ready,1,0");
shouldBe(second.loop(testLoopCount), "ready,2,1");
first.bump();
shouldBe([first.read(), second.read()].join(";"), "ready,1,1;ready,2,1");
