// Bun: this fork's test. A load by name (an import()) has its registry entry from the start of its fetch. Until the
// fetched source arrives the entry is New and nothing waits on it: another load by name and a static import that come
// to it fetch for themselves, and all of them get one module. A fetch that fails leaves the key free, once no other
// load by name fetches for it.

function shouldBe(actual, expected, what)
{
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected} but got ${actual}`);
}

const directory = "./resources/bun-module-load-by-name-registers-when-fetch-starts/";
const dependency = directory + "dependency.js";
const importer = directory + "importer.js";
const missing = directory + "no-such-file.js";

let outcome = "did not finish";
(async function () {
    shouldBe($vm.moduleRegistryStatus(dependency), undefined, "before the load");
    const first = import(dependency);
    shouldBe($vm.moduleRegistryStatus(dependency), "New", "when import() has returned");
    const second = import(dependency);
    shouldBe(await first, await second, "two loads of one name");
    shouldBe($vm.moduleRegistryStatus(dependency), "Fetched", "after the load");
    shouldBe((await first).evaluations, 1, "evaluations");

    const failed = import(missing).then(() => "loaded", () => "rejected");
    shouldBe($vm.moduleRegistryStatus(missing), "New", "when import() of a file that is not there has returned");
    shouldBe(await failed, "rejected", "the load of a file that is not there");
    shouldBe($vm.moduleRegistryStatus(missing), undefined, "after the fetch failed");

    // A load of that name starts between the failure of another one's fetch and the report of that failure. The error is
    // not stored in its entry.
    let startedMeanwhile;
    const earlier = import(missing).then(() => "loaded", () => "rejected");
    Promise.resolve().then(() => { startedMeanwhile = import(missing).then(() => "loaded", () => "rejected"); });
    shouldBe(await earlier, "rejected", "the load whose failure is reported");
    shouldBe(await startedMeanwhile, "rejected", "the load that started meanwhile");
    shouldBe($vm.moduleRegistryStatus(missing), undefined, "after the load that started meanwhile failed");

    // Two loads of that name overlap. The job between theirs runs when the fetch of the first has failed.
    let statusBetween = "not read";
    const failedFirst = import(missing).then(() => "loaded", () => "rejected");
    Promise.resolve().then(() => { statusBetween = $vm.moduleRegistryStatus(missing); });
    const failedSecond = import(missing).then(() => "loaded", () => "rejected");
    shouldBe(await failedFirst, "rejected", "the first of two loads of a file that is not there");
    shouldBe(await failedSecond, "rejected", "the second of two loads of a file that is not there");
    shouldBe(statusBetween, "New", "after the first of two fetches failed");
    shouldBe($vm.moduleRegistryStatus(missing), undefined, "after both fetches failed");

    // A static import comes to the key at each distance from the load by name, in a loader of its own each time. At one
    // of them the entry is still New when the static import comes to it.
    let loadByNameWasFirst = false;
    let staticImportWasFirst = false;
    for (let jobs = 0; jobs < 16; jobs++) {
        const loader = $vm.createModuleLoader();
        const graph = $vm.moduleLoaderImport(loader, importer);
        for (let i = 0; i < jobs; i++)
            await undefined;
        const registered = $vm.moduleRegistryStatus(dependency, loader) !== undefined;
        const byName = $vm.moduleLoaderImport(loader, dependency);
        if (registered)
            staticImportWasFirst = true;
        else {
            loadByNameWasFirst = true;
            shouldBe($vm.moduleRegistryStatus(dependency, loader), "New", `the load by name, ${jobs} jobs after the importer`);
        }
        shouldBe((await graph).dependency, await byName, `one module, ${jobs} jobs after the importer`);
        shouldBe((await byName).evaluations, jobs + 2, `evaluations, ${jobs} jobs after the importer`);
    }
    shouldBe(loadByNameWasFirst && staticImportWasFirst, true, "the static import came to the key behind the load by name, and ahead of it");
    outcome = "passed";
}()).catch(error => { outcome = error; });

drainMicrotasks();
if (outcome !== "passed")
    throw outcome;
