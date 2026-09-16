function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`bad value: expected ${expected} but got ${actual}`);
}

// What the jobs of an earlier turn of the run loop held is collected by a collection made from a later turn.
//
// Every turn runs its jobs from a microtask checkpoint at the same stack depth, so runInternalMicrotask()'s frame is at the
// same address each time, and a job writes only the slots that its own path through that function uses. main() below is
// resumed by the AsyncFunctionResume job and collects from under it. The garbage is made by jobs of other kinds (a promise
// reaction that returns the object, the resumptions of an async generator that yields it): without a checkpoint that clears
// the stack its frames are going to occupy, what those left in the frame is a root of every collection main() makes.

const turn = () => new Promise(resolve => setTimeout(resolve, 0));

const refs = [];

function viaPromiseReaction()
{
    return turn().then(() => {
        const object = { };
        refs.push(new WeakRef(object));
        return object;
    }).then(() => { });
}

async function* generator()
{
    for (let i = 0; i < 2; ++i) {
        const object = { i };
        refs.push(new WeakRef(object));
        await turn();
        yield object;
    }
}

async function viaAsyncGenerator()
{
    for await (const object of generator())
        shouldBe(typeof object.i, "number");
}

async function main()
{
    for (const makeGarbage of [viaPromiseReaction, viaAsyncGenerator]) {
        refs.length = 0;
        await makeGarbage();

        // A WeakRef keeps its target until the end of the turn that made it or read it, so each collection is in a turn
        // of its own. The scan is conservative: a few turns for anything else that happens to look like one of the
        // objects. What the checkpoint's frames keep, they keep in every turn.
        let alive;
        for (let turns = 0; turns < 10; ++turns) {
            await turn();
            fullGC();
            alive = refs.filter(ref => ref.deref()).length;
            if (!alive)
                break;
        }
        shouldBe(alive, 0);
    }
}

asyncTestStart(1);
main().then(asyncTestPassed, error => {
    // Without asyncTestPassed() the shell exits with a failure.
    print(String(error));
});
