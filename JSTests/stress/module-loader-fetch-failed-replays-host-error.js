//@ runDefault

// A module whose fetch was rejected while another module imported it keeps a
// FetchFailed registry entry, and the load of every later importer is settled
// from that entry by JSModuleLoader::hostLoadImportedModule(). It used to hand
// out JSModuleLoader::duplicateError()'s copy of the stored error, which keeps
// only its type and message. That copy exists for errors the host marked as
// fetch failures (moduleFetchFailureKind); an error the host rejected with
// itself, like the one this shell creates for a missing file, must be replayed
// unchanged, as the other replay sites (maybeDuplicateFetchError) already do.

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
    const first = await shouldReject(import("./resources/module-fetch-failed-replay/importer-a.js"));
    if (!String(first).includes("does-not-exist"))
        throw new Error("importer-a: unexpected error: " + first);
    first.seenByFirstImporter = true;

    const second = await shouldReject(import("./resources/module-fetch-failed-replay/importer-b.js"));
    if (second.message !== first.message)
        throw new Error("importer-b: unexpected error: " + second);
    if (second !== first || !second.seenByFirstImporter)
        throw new Error("importer-b: rejected with a copy of the stored fetch error instead of the error itself");
}()).catch((error) => {
    print(String(error));
    print(error.stack);
    abort();
});
