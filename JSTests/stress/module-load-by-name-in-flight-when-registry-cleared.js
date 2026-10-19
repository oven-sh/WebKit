// Bun: this fork's test. clearModuleRegistry() is JSModuleLoader::clearAll(), which an embedder calls to load every
// module again. A load by name (import()) is not in the registry until its fetch settles, so a clear cannot take it
// out. It ends on an entry the registry does not hold: whoever awaits it gets its module, and the next import() of the
// key loads again.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

const settled = promise => promise.then(module => module.evaluation, error => "rejected: " + error.message);

let outcome = "did not finish";
(async function () {
    // Two loads in flight at the clear share a module, as they would have in the registry.
    const first = import("./resources/module-load-stale/counts-evaluations.js");
    const second = import("./resources/module-load-stale/counts-evaluations.js");
    clearModuleRegistry();
    const [a, b] = await Promise.all([first, second]);
    shouldBe(a, b);
    shouldBe(a.evaluation, 1);

    const next = await import("./resources/module-load-stale/counts-evaluations.js");
    shouldBe(next === a, false);
    shouldBe(next.evaluation, 2);
    shouldBe(await import("./resources/module-load-stale/counts-evaluations.js"), next);

    // A load that joined a registered entry goes on with that entry.
    const joined = import("./resources/module-load-stale/counts-evaluations.js");
    clearModuleRegistry();
    shouldBe(await joined, next);
    shouldBe((await import("./resources/module-load-stale/counts-evaluations.js")).evaluation, 3);

    // A load in flight at the clear that fails does not leave its error on the module loaded after the clear.
    globalThis.evaluations = 0;
    globalThis.throwFromModule = true;
    const failing = settled(import("./resources/module-load-stale/throws.js"));
    clearModuleRegistry();
    shouldBe(await failing, "rejected: thrown by the module");
    globalThis.throwFromModule = false;
    shouldBe(await settled(import("./resources/module-load-stale/throws.js")), 2);
    shouldBe(await settled(import("./resources/module-load-stale/throws.js")), 2);

    outcome = "passed";
}()).catch(error => { outcome = error; });

drainMicrotasks();
if (outcome !== "passed")
    throw outcome;
