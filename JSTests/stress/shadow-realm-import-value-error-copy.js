//@ requireOptions("--useShadowRealm=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

async function rejectionOf(promise) {
    try {
        await promise;
    } catch (error) {
        return error;
    }
    throw new Error('did not reject');
}

const importFallback = "Error encountered during evaluation";

asyncTestStart(1);
(async function () {
    let realm = new ShadowRealm();

    // The module throws a value with nothing to describe it, and converting it runs code of the
    // realm that throws the realm's globalThis. The caller gets a TypeError of its own realm, and
    // the conversion never runs.
    {
        let error = await rejectionOf(realm.importValue('./resources/shadow-realm-module-throws-unconvertible.js', 'value'));
        shouldBe(error instanceof TypeError, true);
        shouldBe($.globalObjectFor(error), globalThis);
        shouldBe(error.message, importFallback);
        // The realm's globalThis would answer with the realm's Array.
        shouldBe(typeof error.Array, 'undefined');
        shouldBe(realm.evaluate(`globalThis.ran`), false);
    }

    // An Error of the realm still describes itself with its own message.
    {
        let error = await rejectionOf(realm.importValue('./resources/shadow-realm-module-throws-error.js', 'value'));
        shouldBe(error instanceof TypeError, true);
        shouldBe($.globalObjectFor(error), globalThis);
        shouldBe(error.message, 'boom');
    }

    // The cached failure of the same module translates the same way.
    {
        let error = await rejectionOf(realm.importValue('./resources/shadow-realm-module-throws-error.js', 'value'));
        shouldBe(error instanceof TypeError, true);
        shouldBe(error.message, 'boom');
    }

    // A specifier that does not resolve keeps the module loader's own message.
    {
        let error = await rejectionOf(realm.importValue('./resources/this-module-does-not-exist.js', 'value'));
        shouldBe(error instanceof TypeError, true);
        shouldBe($.globalObjectFor(error), globalThis);
        shouldBe(error.message.length > 0, true);
    }

    asyncTestPassed();
}()).catch((error) => {
    print(String(error));
    $vm.abort();
});
