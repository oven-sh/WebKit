// The bytecode-cache mode runs a test twice and the second time takes all code from the cache of the first. The module
// this test writes has a new name on every run, so it is never in that cache.
//@ $skipModes << "bytecode-cache".to_sym

// Bun: this fork's test. Two loads by name of one key overlap, and the first fails at its fetch because the file is not
// there yet. The first load has no registry entry, so its error must not be stored into the entry the second load
// registered: a later import() of the key gets the module.
//
// The test writes the module it imports into resources/, under a name of its own for every run.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

// writeFile() takes a path as the shell was given this file's; import() resolves against this file.
const directory = /@(.*?)[^\/\\]*:\d+:\d+$/m.exec(new Error().stack)[1];
const name = `resources/module-load-failed-fetch-${Date.now()}-${Math.floor(Math.random() * 1e9)}.generated.js`;

const settled = promise => promise.then(module => module.value, error => "rejected");

let outcome = "did not finish";
(async function () {
    const first = settled(import(`./${name}`));
    writeFile(directory + name, "export const value = 42;");
    const second = import(`./${name}`);

    shouldBe(await first, "rejected");
    // Whether the second load shares the first one's fetch is not what this tests.
    await second.catch(() => { });
    shouldBe(await settled(import(`./${name}`)), 42);
    outcome = "passed";
}()).catch(error => { outcome = error; });

drainMicrotasks();
if (outcome !== "passed")
    throw outcome;
