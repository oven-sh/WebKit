// JSPromise::maker(): once promises remember their maker, a promise that is rejected with nothing handling it
// names the script that made it, whoever rejected it, in every tier.
import { shouldBe } from "./resources/assert.js";

const before = Promise.withResolvers();
$vm.promisesRememberTheirMaker();

const first = $vm.createModuleLoader({ owner: "A" });
const A = await $vm.moduleLoaderImport(first, "./promise-maker/code.js");
// The same code as A's: the two loaders' modules share executables.
const B = await $vm.moduleLoaderImport($vm.createModuleLoader({ owner: "B" }, first), "./promise-maker/code.js");
const G = await import("./promise-maker/code.js");
const all = [[A, "A"], [B, "B"], [G, undefined]];

// What a rejected promise that nothing handles says about who made it, after the jobs that reject it have run.
async function makerOf(promise) {
    for (let i = 0; i < 4; ++i)
        await null;
    const owner = $vm.ownerOfMaker(promise);
    promise.catch(() => { });
    return owner;
}

for (let i = 0; i < testLoopCount; ++i) {
    for (const [maker, owner] of all) {
        for (const [rejecter] of all) {
            // Rejected by another loader's script, through the promise's own reject function.
            for (const make of [maker.withResolvers, maker.byConstructor]) {
                const made = make();
                rejecter.callsReject(made.reject);
                shouldBe(await makerOf(made.promise), owner);
            }
            // An async function's promise is the function's, whoever's script throws inside it.
            shouldBe(await makerOf(maker.asyncCalling(rejecter.thrower)), owner);
            // A promise derived from another loader's promise is the script's that derived it.
            for (const derive of [maker.thenOf, maker.finallyOf, maker.all, maker.race, maker.resolvedWith, maker.asyncAwaiting]) {
                const source = rejecter.withResolvers();
                const derived = derive(source.promise);
                rejecter.callsReject(source.reject);
                shouldBe(await makerOf(derived), owner);
            }
            {
                const source = rejecter.withResolvers();
                const derived = maker.thenThatThrows(source.promise);
                source.resolve();
                shouldBe(await makerOf(derived), owner);
            }
        }
        shouldBe(await makerOf(maker.rejected()), owner);
        shouldBe(await makerOf(maker.asyncThatThrows()), owner);
        shouldBe(await makerOf(maker.asyncThatThrowsAfterAwait()), owner);
        shouldBe(await makerOf(maker.generatorNext()), owner);
        // A promise nothing has been done with says who made it; one with a reaction, or fulfilled, says nothing.
        const pending = maker.pending();
        shouldBe($vm.ownerOfMaker(pending), owner);
        pending.then(() => { });
        shouldBe($vm.ownerOfMaker(pending), "none");
        shouldBe($vm.ownerOfMaker(Promise.resolve(1)), "none");
    }
}

// A promise made before promises remembered their maker has none.
before.reject(new Error("rejected"));
shouldBe(await makerOf(before.promise), "none");
