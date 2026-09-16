import { shouldBe } from "./resources/assert.js";

// What a finished async function held is collected by a collection made from a later turn of the run loop.
//
// Every turn resumes makeGarbage(), and later this module, from a microtask checkpoint at the same stack depth, so
// runInternalMicrotask()'s frame is at the same address each time. The job that resumes an async function leaves the
// function's generator in a slot of that frame. The job that resumes a module does not write that slot, and the collections
// below are made from under it: without a checkpoint that clears the stack its frames are going to occupy, the conservative
// scan finds the generator, and through it `list` and the objects, in every turn.

const turn = () => new Promise(resolve => setTimeout(resolve, 0));

const refs = [];
async function makeGarbage()
{
    const list = [];
    for (let i = 0; i < 2; ++i) {
        const object = { i };
        list.push(object);
        refs.push(new WeakRef(object));
        await turn();
    }
    await Promise.all(list.map(object => Promise.resolve(object.i)));
}

await makeGarbage();

// A WeakRef keeps its target until the end of the turn that made it or read it, so each collection is in a turn of its own.
// The scan is conservative: a few turns for anything else that happens to look like one of the objects. What the
// checkpoint's frames keep, they keep in every turn.
let alive;
for (let turns = 0; turns < 10; ++turns) {
    await turn();
    fullGC();
    alive = refs.filter(ref => ref.deref()).length;
    if (!alive)
        break;
}
shouldBe(alive, 0);
