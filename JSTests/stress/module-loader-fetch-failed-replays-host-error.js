//@ runDefault

// A module whose fetch was rejected keeps a FetchFailed registry entry, and every
// later load of it is settled from that entry: top-level loads through
// ModuleRegistryEntry::error(), loads on behalf of an importing module through
// JSModuleLoader::hostLoadImportedModule(). Both used to hand out
// JSModuleLoader::duplicateError()'s copy of the stored error, which keeps only
// its type and message. That copy exists for errors the host marked as fetch
// failures (moduleFetchFailureKind); an error the host rejected with itself, like
// the one this shell creates for a missing file, must be replayed unchanged, as
// the other replay sites (maybeDuplicateFetchError) already do.

var abort = $vm.abort;

async function shouldReject(promise) {
    try {
        await promise;
    } catch (e) {
        return e;
    }
    throw new Error("did not reject");
}

(async function () {
    const missing = "./resources/module-fetch-failed-replay/does-not-exist.js";

    const first = await shouldReject(import(missing));
    if (!String(first).includes("does-not-exist"))
        throw new Error("unexpected error: " + first);

    const replays = {
        // Settled from ModuleRegistryEntry::error().
        topLevel: await shouldReject(import(missing)),
        // Settled from hostLoadImportedModule() while loading the importer's graph.
        importerA: await shouldReject(import("./resources/module-fetch-failed-replay/importer-a.js")),
        importerB: await shouldReject(import("./resources/module-fetch-failed-replay/importer-b.js")),
    };

    for (const [name, error] of Object.entries(replays)) {
        if (error.message !== first.message)
            throw new Error(name + ": unexpected error: " + error);
        if (error !== first)
            throw new Error(name + ": rejected with a copy of the stored fetch error instead of the error itself");
    }
}()).catch((error) => {
    print(String(error));
    print(error.stack);
    abort();
});
