// JSPromise::madeFor(): a promise does not keep the function it was made for alive, and still names who it was
// made for once the function is gone.
import { shouldBe } from "./resources/assert.js";

$vm.promisesAreMadeForOwners();

// (The owners are numbers: what is kept of a function once a collection has seen its promise is not a cell.)
const first = $vm.createModuleLoader({ owner: 1 });
const A = await $vm.moduleLoaderImport(first, "./promise-maker/code.js");
const G = await import("./promise-maker/code.js");

// (A WeakRef's target is kept until the jobs that are queued have run: so from a task of its own.)
const task = () => new Promise(resolve => setTimeout(resolve, 0));
async function collect() {
    for (let i = 0; i < 3; ++i) {
        await task();
        fullGC();
    }
    await task();
}

for (const [maker, owner] of [[A, 1], [G, undefined]]) {
    // Something only the executor has is collected while the promise is pending: with nothing done with the
    // promise, and with reactions. (Not every time: what has just run can be kept by what ran it.)
    for (const react of [false, true]) {
        const made = [];
        for (let i = 0; i < testLoopCount; ++i) {
            made.push(maker.constructedHolding());
            if (react) {
                made[i].promise.then(() => { }, () => { });
                made[i].promise.then(() => { }, () => { });
            }
        }
        await collect();
        shouldBe(made.some(each => each.held.deref() === undefined), true);
        if (react)
            continue;
        // The promises still say who they were made for, before and after they are rejected.
        for (const each of made) {
            shouldBe($vm.ownerOfMaker(each.promise), owner);
            G.callsReject(each.reject);
        }
        await collect();
        for (const each of made) {
            shouldBe($vm.ownerOfMaker(each.promise), owner);
            each.promise.catch(() => { });
        }
    }

    // The promise `finally` made, while it waits for what its handler returned.
    const waiting = [];
    for (let i = 0; i < testLoopCount; ++i) {
        const source = Promise.withResolvers();
        waiting.push(maker.finallyHolding(source.promise));
        source.resolve();
    }
    await collect();
    for (const each of waiting) {
        shouldBe($vm.ownerOfMaker(each.promise), owner);
        G.callsReject(each.returned.reject);
    }
    await collect();
    for (const each of waiting) {
        shouldBe($vm.ownerOfMaker(each.promise), owner);
        each.promise.catch(() => { });
    }
}
