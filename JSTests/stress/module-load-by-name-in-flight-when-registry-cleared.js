// The bytecode-cache mode runs a test twice and the second time takes all code from the cache of the first. The modules
// this test writes have new names on every run, so they are never in that cache.
//@ $skipModes << "bytecode-cache".to_sym

// Bun: this fork's test. clearModuleRegistry() is JSModuleLoader::clearAll(), which an embedder calls to load every
// module again. A load by name (import()) is not in the registry until its fetch settles, so a clear cannot take it
// out. What its fetch settles with is from before the clear: the load does not use it, and starts again.
//
// The shell fetches when import() is called. A file that the test writes again between the import() and the clear
// shows which fetch a module came from. The test writes its modules into resources/, under names of their own for
// every run.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

const resources = "resources/module-load-by-name-in-flight-when-registry-cleared/";
// writeFile() takes a path as the shell was given this file's; import() resolves against this file.
const directory = /@(.*?)[^\/\\]*:\d+:\d+$/m.exec(new Error().stack)[1] + resources;
const run = `${Date.now()}-${Math.floor(Math.random() * 1e9)}`;
const nameOf = label => `${label}-${run}.generated.js`;
const write = (name, value) => writeFile(directory + name, `globalThis.evaluated.push(${value}); export const value = ${value};`);
const load = name => import(`./${resources}${name}`);

globalThis.evaluated = [];

let outcome = "did not finish";
(async function () {
    // The fetch of a load by name is in flight at the clear. Two such loads of one key end with one module.
    const own = nameOf("own");
    write(own, 1);
    const first = load(own);
    const second = load(own);
    write(own, 2);
    clearModuleRegistry();
    const [a, b] = await Promise.all([first, second]);
    shouldBe(a.value, 2);
    shouldBe(a, b);
    shouldBe(await load(own), a);
    shouldBe(globalThis.evaluated.join(), "2");

    // A load by name that joined the fetch of a module in the registry.
    const joined = load(own);
    write(own, 3);
    clearModuleRegistry();
    shouldBe((await joined).value, 3);
    shouldBe(globalThis.evaluated.join(), "2,3");

    // The fetch failed: the file was not there. That failure is from before the clear too.
    const late = nameOf("late");
    const failed = load(late);
    write(late, 4);
    clearModuleRegistry();
    shouldBe((await failed).value, 4);
    shouldBe(globalThis.evaluated.join(), "2,3,4");

    // A second clear overtakes the fetch that the load made when it started again. The microtask is queued behind
    // the settle of the first fetch, so it runs between that settle and the settle of the second fetch.
    const twice = nameOf("twice");
    write(twice, 5);
    const overtakenTwice = load(twice);
    write(twice, 6);
    Promise.resolve().then(() => {
        write(twice, 7);
        clearModuleRegistry();
    });
    clearModuleRegistry();
    shouldBe((await overtakenTwice).value, 7);
    shouldBe(await load(twice), await overtakenTwice);
    shouldBe(globalThis.evaluated.join(), "2,3,4,7");

    // The fetch that the load makes when it starts again fails. The import() rejects, and nothing of the failure is
    // left on the key: a module that imports the key loads the file that is there by then.
    const absent = nameOf("absent");
    const importer = nameOf("importer");
    const rejected = load(absent).then(() => "loaded", () => "rejected");
    clearModuleRegistry();
    shouldBe(await rejected, "rejected");
    write(absent, 8);
    writeFile(directory + importer, `export { value } from "./${absent}";`);
    shouldBe((await load(importer)).value, 8);
    shouldBe(globalThis.evaluated.join(), "2,3,4,7,8");

    // A clear with no fetch in flight changes nothing for the loads that start after it.
    const missing = nameOf("missing");
    shouldBe(await load(missing).then(() => "loaded", () => "rejected"), "rejected");
    write(missing, 9);
    shouldBe((await load(missing)).value, 9);
    shouldBe(await load(missing), await load(missing));
    shouldBe(globalThis.evaluated.join(), "2,3,4,7,8,9");

    outcome = "passed";
}()).catch(error => { outcome = error; });

drainMicrotasks();
if (outcome !== "passed")
    throw outcome;
