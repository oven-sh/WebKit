//@ requireOptions("--useShadowRealm=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

async function shouldRejectWithTypeError(promise, expectedMessage) {
    let error;
    let rejected = false;
    try {
        await promise;
    } catch (e) {
        rejected = true;
        error = e;
    }
    if (!rejected)
        throw new Error("did not reject");
    if (!(error instanceof TypeError))
        throw new Error(`expected a TypeError from the incubating realm but got ${String(error)}`);
    shouldBe($.globalObjectFor(error), globalThis);
    shouldBe(error.message, expectedMessage);
}

const importFallback = "Error encountered during import";

asyncTestStart(1);
(async function () {
    let realm = new ShadowRealm();

    // A module that throws the empty string leaves the rejection with nothing to describe. The
    // caller still gets a TypeError with a message. The second import reads the module loader's
    // cache of the same failure.
    const emptyString = "./resources/shadow-realm-throw-empty-string-module.js";
    await shouldRejectWithTypeError(realm.importValue(emptyString, "value"), importFallback);
    await shouldRejectWithTypeError(realm.importValue(emptyString, "value"), importFallback);

    // The same for a thrown value that describes itself with the empty string.
    const emptyDescription = "./resources/shadow-realm-throw-empty-description-module.js";
    await shouldRejectWithTypeError(realm.importValue(emptyDescription, "value"), importFallback);

    // A description that is not empty still reaches the caller.
    await shouldRejectWithTypeError(realm.importValue("./resources/shadow-realm-throw-error-module.js", "value"), "Error: boom");

    // A module that does not exist keeps the module loader's own message.
    let error;
    try {
        await realm.importValue("./resources/shadow-realm-no-such-module.js", "value");
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error(`expected a TypeError but got ${String(error)}`);
    if (!error.message.includes("shadow-realm-no-such-module.js"))
        throw new Error(`expected the module loader's message but got ${error.message}`);

    asyncTestPassed();
}()).catch((error) => {
    print(String(error));
    $vm.abort();
});
