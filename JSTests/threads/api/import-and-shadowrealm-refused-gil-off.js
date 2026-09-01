//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--useShadowRealm=1")
// GIL-off, a spawned Thread may neither start a module graph load (dynamic
// import() rejects with a TypeError before the loader hook is consulted) nor
// boot a fresh realm (new ShadowRealm() throws a TypeError before the
// deriveShadowRealmGlobalObject hook runs). The main thread keeps both: its
// import() of a missing module still reaches the shell loader, which rejects
// with a plain Error, and new ShadowRealm() succeeds.
load("../harness.js", "caller relative");

asyncTestStart(1);

shouldNotThrow(() => new ShadowRealm(), "new ShadowRealm() on the main thread");

import("./does-not-exist.js").then(
    () => { throw new Error("main-thread import() of a missing module fulfilled"); },
    e => {
        shouldBeFalse(e instanceof TypeError, "main-thread import() reaches the loader: " + String(e));
        asyncTestPassed();
    });

const box = { import: "pending" };
const spawnedRealmOutcome = new Thread(() => {
    import("./does-not-exist.js").then(
        () => { box.import = "fulfilled"; },
        e => { box.import = e instanceof TypeError ? "TypeError" : String(e); });
    try {
        new ShadowRealm();
        return "no-throw";
    } catch (e) {
        return e instanceof TypeError ? "TypeError" : String(e);
    }
}).join();

shouldBe(spawnedRealmOutcome, "TypeError", "new ShadowRealm() on a spawned thread");
// The spawned thread drains its own microtask queue before join() publishes
// its result, so the import() reaction has already run.
shouldBe(box.import, "TypeError", "import() on a spawned thread");
