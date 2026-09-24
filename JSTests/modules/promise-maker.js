// JSPromise::whose(): a promise that is rejected with nothing handling it says whose the function it was made for
// is, whoever rejected it, in every tier.
import { shouldBe } from "./resources/assert.js";

// Until the embedder asks for it, promises keep nothing.
let rejectBefore;
const before = new Promise((_, reject) => { rejectBefore = reject; });
shouldBe($vm.ownerOfMaker(before), "none");
$vm.promisesAreMadeForOwners();

// (The owners are numbers: what a promise keeps is not a cell.)
const first = $vm.createModuleLoader({ owner: 1 });
const A = await $vm.moduleLoaderImport(first, "./promise-maker/code.js");
// The same code as A's: the two loaders' modules share executables.
const B = await $vm.moduleLoaderImport($vm.createModuleLoader({ owner: 2 }, first), "./promise-maker/code.js");
const G = await import("./promise-maker/code.js");
const all = [[A, 1], [B, 2], [G, undefined]];

// A script under no scope that says whose it is: its own scopes are not taken for one.
const makeInHostScript = (0, eval)("(function () { let captured = { }; return new Promise(() => captured); })");

// What a rejected promise that nothing handles says about who it was made for, after the jobs that reject it
// have run.
async function makerOf(promise) {
    for (let i = 0; i < 4; ++i)
        await null;
    const owner = $vm.ownerOfMaker(promise);
    promise.catch(() => { });
    return owner;
}

for (let i = 0; i < testLoopCount; ++i) {
    for (const [maker, owner] of all) {
        for (const [rejecter, rejecterOwner] of all) {
            // A promise is whose its executor is, whoever's script constructs it.
            shouldBe(await makerOf(maker.constructedWith(rejecter.rejectingExecutor)), rejecterOwner);
            // Rejected by another loader's script, through the promise's own reject function.
            {
                const made = maker.byConstructor();
                rejecter.callsReject(made.reject);
                shouldBe(await makerOf(made.promise), owner);
            }
            // An async function's promise is the function's, whoever's script throws inside it.
            shouldBe(await makerOf(maker.asyncCalling(rejecter.thrower)), owner);
            // A promise derived from another loader's promise is made for the function it was derived with.
            for (const derive of [maker.thenOf, maker.finallyOf, maker.constructedResolvedWith, maker.asyncAwaiting, maker.asyncReturning]) {
                const source = rejecter.withResolvers();
                const derived = derive(source.promise);
                rejecter.callsReject(source.reject);
                shouldBe(await makerOf(derived), owner);
            }
            for (const derive of [maker.thenThatThrows, maker.thenReturningRejected]) {
                const source = rejecter.withResolvers();
                const derived = derive(source.promise);
                source.resolve();
                shouldBe(await makerOf(derived), owner);
            }
            // Made for no function.
            for (const derive of [maker.all, maker.race, maker.resolvedWith]) {
                const source = rejecter.withResolvers();
                const derived = derive(source.promise);
                rejecter.callsReject(source.reject);
                shouldBe(await makerOf(derived), "none");
            }
            {
                const made = maker.withResolvers();
                rejecter.callsReject(made.reject);
                shouldBe(await makerOf(made.promise), "none");
            }
        }
        shouldBe(await makerOf(maker.asyncThatThrowsAfterAwait()), owner);
        shouldBe(await makerOf(maker.generatorNext()), owner);
        shouldBe(await makerOf(maker.constructedThatThrows()), owner);
        shouldBe(await makerOf(maker.constructedOneScopeDown()), owner);
        shouldBe(await makerOf(maker.constructedTwoScopesDown()), owner);
        shouldBe(await makerOf(maker.constructedWithGiven()), owner);
        shouldBe(await makerOf(maker.constructedWithBound()), owner);
        shouldBe(await makerOf(maker.constructedByDerived()), owner);
        shouldBe(await makerOf(maker.rejected()), "none");
        // A promise nothing has been done with says who it was made for; one with a reaction, or fulfilled, says
        // nothing.
        const pending = maker.pending();
        shouldBe($vm.ownerOfMaker(pending), owner);
        pending.then(() => { });
        shouldBe($vm.ownerOfMaker(pending), "none");
        shouldBe($vm.ownerOfMaker(maker.fulfilled()), "none");
        shouldBe($vm.ownerOfMaker(Promise.resolve(1)), "none");
    }
}

for (let i = 0; i < testLoopCount; ++i)
    shouldBe($vm.ownerOfMaker(makeInHostScript()), undefined);

rejectBefore(new Error("rejected"));
shouldBe(await makerOf(before), "none");
