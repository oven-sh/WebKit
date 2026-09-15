//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=0")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useCodeRecoveryFromBytecodeCache=0")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useRunOnceCodeRelease=0")
//@ runBytecodeCache

// Module code that has run is released (useRunOnceCodeRelease; the unlinked code too if it can be decoded again from a
// persistent bytecode cache payload). Records of several module loaders share one ModuleProgramExecutable: the code goes
// when the last of them has finished, a loader that comes later adopts the executable and has the code decoded again, and
// the function declarations' executables stay shared throughout.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const options = jscOptions();
const staysInterpreted = options.useLLInt && options.thresholdForJITAfterWarmUp >= 100 && options.thresholdForJITSoon >= 100;
const releasesLinkedCode = staysInterpreted && options.useRunOnceCodeRelease;
const releasesUnlinkedCode = releasesLinkedCode && options.useCodeRecoveryFromBytecodeCache && options.useLeanBytecodeCacheDecoder
    && options.useBorrowedBytecodeFromCache && options.diskCachePayloadIsPersistentForTesting && options.forceDiskCache;
// What the executables hold, not how many code blocks are alive: the collector scans the stack conservatively, and a word
// left behind in a frame of the run loop can keep a code block that nothing refers to any more for a while.
const census = () => { fullGC(); return $vm.codeBlockCensus(); };
const load = (path) => $vm.moduleLoaderImport($vm.createModuleLoader(), path);

async function test() {
    const lib = "./resources/module-loaders-released-code/lib.js";
    const a = await load(lib);
    assert(a.marker === 11, "a ran");
    let now = census();
    if (releasesLinkedCode)
        assert(now.moduleExecutablesWithLinkedCode === 0, "a's module code was released: " + JSON.stringify(now));
    if (releasesUnlinkedCode)
        assert(now.moduleExecutablesWithUnlinkedCode === 0, "and its unlinked code: " + JSON.stringify(now));
    assert(a.late(2) === 4, "a.late");

    // The second loader adopts the executable, released code and all.
    const b = await load(lib);
    assert(b !== a && b.marker === 11 && b.callCount() === 1 && a.callCount() === 2, "b has its own state");
    assert(typeof $vm.codeBlockFor(b.late) === "string" && $vm.codeBlockFor(b.late) === $vm.codeBlockFor(a.late), "b.late has the code a.late was given");
    now = census();
    if (releasesLinkedCode)
        assert(now.moduleExecutablesWithLinkedCode === 0, "the module code was released again after b: " + JSON.stringify(now));
    if (releasesUnlinkedCode)
        assert(now.moduleExecutablesWithUnlinkedCode === 0, "and the unlinked code: " + JSON.stringify(now));
    // A declaration whose executable b links first is a's as well.
    assert(b.neverReadByTheFirstLoader(2) === 6, "b.neverReadByTheFirstLoader");
    assert($vm.codeBlockFor(a.neverReadByTheFirstLoader) === $vm.codeBlockFor(b.neverReadByTheFirstLoader), "one executable for both loaders");
    assert(a.neverReadByTheFirstLoader(3) === 9 && a.callCount() === 3 && b.callCount() === 2, "each counts its own calls");
    if (releasesUnlinkedCode)
        assert(census().moduleExecutablesWithUnlinkedCode === 0, "reading declarations did not bring the unlinked code back");

    // One loader is suspended in the module's body while another runs it to the end: the code stays until both are done.
    const tla = "./resources/module-loaders-released-code/tla.js";
    globalThis.moduleLoadersReleasedCodeStarted = 0;
    let openGate;
    globalThis.moduleLoadersReleasedCodeNextGate = new Promise((resolve) => { openGate = resolve; });
    const pending = load(tla);
    while (globalThis.moduleLoadersReleasedCodeStarted < 1)
        await new Promise((resolve) => setTimeout(resolve, 1));
    globalThis.moduleLoadersReleasedCodeNextGate = undefined;
    const d = await load(tla);
    assert(globalThis.moduleLoadersReleasedCodeStarted === 2 && d.stage === "done" && d.describe() === "done:later", "the second loader ran the body to its end");
    now = census();
    if (releasesLinkedCode && !options.useEagerCodeBlockJettisonTiming && !options.forceCodeBlockToJettisonDueToOldAge)
        assert(now.moduleExecutablesWithLinkedCode === 1, "the suspended loader keeps the module's code: " + JSON.stringify(now));
    if (releasesUnlinkedCode)
        assert(now.moduleExecutablesWithUnlinkedCode === 1, "linked and unlinked: " + JSON.stringify(now));
    openGate();
    const c = await pending;
    assert(c !== d && c.stage === "done" && c.describe() === "done:later", "the first loader resumed and finished");
    now = census();
    if (releasesLinkedCode)
        assert(now.moduleExecutablesWithLinkedCode === 0, "now it is released: " + JSON.stringify(now));
    if (releasesUnlinkedCode)
        assert(now.moduleExecutablesWithUnlinkedCode === 0, "linked and unlinked: " + JSON.stringify(now));
    assert($vm.codeBlockFor(c.describe) === $vm.codeBlockFor(d.describe), "shared declarations");

    // A loader whose body throws is done with the code too: a later loader that runs the body to its end releases it.
    const throws = "./resources/module-loaders-released-code/throws.js";
    globalThis.moduleLoadersReleasedCodeThrowsStarted = 0;
    globalThis.moduleLoadersReleasedCodeShouldThrow = true;
    let thrown;
    try {
        await load(throws);
    } catch (error) {
        thrown = error;
    }
    assert(thrown instanceof Error && thrown.message === "the body throws" && globalThis.moduleLoadersReleasedCodeThrowsStarted === 1, "the first loader's body threw");
    globalThis.moduleLoadersReleasedCodeShouldThrow = false;
    const t = await load(throws);
    assert(t.done === true && t.late() === "late" && globalThis.moduleLoadersReleasedCodeThrowsStarted === 2, "the second loader ran it to the end");
    now = census();
    if (releasesLinkedCode)
        assert(now.moduleExecutablesWithLinkedCode === 0, "released although an earlier loader's body threw: " + JSON.stringify(now));
    if (releasesUnlinkedCode)
        assert(now.moduleExecutablesWithUnlinkedCode === 0, "linked and unlinked: " + JSON.stringify(now));

    // So is a loader's record that never runs because a module it depends on threw,
    const parent = "./resources/module-loaders-released-code/parent-of-throwing.js";
    globalThis.moduleLoadersReleasedCodeParentStarted = 0;
    globalThis.moduleLoadersReleasedCodeShouldThrow = true;
    thrown = undefined;
    try {
        await load(parent);
    } catch (error) {
        thrown = error;
    }
    assert(thrown instanceof Error && thrown.message === "the dependency throws" && globalThis.moduleLoadersReleasedCodeParentStarted === 0, "the importer never ran");
    globalThis.moduleLoadersReleasedCodeShouldThrow = false;
    const p = await load(parent);
    assert(p.late() === "late:7" && globalThis.moduleLoadersReleasedCodeParentStarted === 1, "the second loader ran the importer");
    now = census();
    if (releasesLinkedCode)
        assert(now.moduleExecutablesWithLinkedCode === 0, "released although an earlier loader's record of it never ran: " + JSON.stringify(now));

    // and one that is suspended at a top-level await when a sibling of it throws: it is never resumed.
    const cycleRoot = "./resources/module-loaders-released-code/cycle-root.js";
    globalThis.moduleLoadersReleasedCodeCycleRootStarted = 0;
    globalThis.moduleLoadersReleasedCodeShouldThrow = true;
    let openSecondGate;
    globalThis.moduleLoadersReleasedCodeNextGate = new Promise((resolve) => { openSecondGate = resolve; });
    thrown = undefined;
    try {
        await load(cycleRoot);
    } catch (error) {
        thrown = error;
    }
    assert(thrown instanceof Error && thrown.message === "the sibling throws" && globalThis.moduleLoadersReleasedCodeCycleRootStarted === 0, "the root of the cycle never ran");
    openSecondGate();
    await new Promise((resolve) => setTimeout(resolve, 1));
    globalThis.moduleLoadersReleasedCodeShouldThrow = false;
    globalThis.moduleLoadersReleasedCodeNextGate = undefined;
    const r = await load(cycleRoot);
    assert(r.describe() === "awaited:sibling" && globalThis.moduleLoadersReleasedCodeCycleRootStarted === 1, "the second loader ran the cycle");
    now = census();
    if (releasesLinkedCode)
        assert(now.moduleExecutablesWithLinkedCode === 0, "released although an earlier loader left a record suspended: " + JSON.stringify(now));

    // All code is deleted between two loaders: the second one's environment is made from another symbol table than the
    // first one's, so what the second links (and the optimizing tiers specialize on its one environment) must not become
    // the code of declarations the first has not read yet.
    const iterations = testLoopCount;
    const checkIsolated = (first, second, what) => {
        let last = 0;
        for (let i = 0; i < iterations; ++i)
            last = second.never();
        for (let i = 0; i < iterations; ++i)
            last = second.never2();
        assert(last === 1 + 2 * iterations, what + ": the second loader counts its own calls, " + last);
        for (let i = 0; i < iterations; ++i)
            last = first.never();
        assert(last === 1 + iterations, what + ": the first loader's never() counts the first loader's calls, " + last);
        for (let i = 0; i < iterations; ++i)
            last = first.never2();
        assert(last === 1 + 2 * iterations, what + ": the first loader's never2() counts the first loader's calls, " + last);
        assert(second.never() === 2 + 2 * iterations, what + ": the second loader is unaffected");
    };
    const lazy = "./resources/module-loaders-released-code/lazy.js";
    const e = await load(lazy);
    assert(e.marker === 7, "e ran");
    $vm.deleteAllCodeWhenIdle();
    await new Promise((resolve) => setTimeout(resolve, 1));
    fullGC();
    const f = await load(lazy);
    checkIsolated(e, f, "deleted after evaluation");

    // The same with a loader that was suspended in the body when the code was deleted, and generated it again to resume.
    const lazyTLA = "./resources/module-loaders-released-code/lazy-tla.js";
    globalThis.moduleLoadersReleasedCodeStarted = 0;
    globalThis.moduleLoadersReleasedCodeNextGate = new Promise((resolve) => { openGate = resolve; });
    const suspended = load(lazyTLA);
    while (globalThis.moduleLoadersReleasedCodeStarted < 1)
        await new Promise((resolve) => setTimeout(resolve, 1));
    $vm.deleteAllCodeWhenIdle();
    await new Promise((resolve) => setTimeout(resolve, 1));
    fullGC();
    openGate();
    const g = await suspended;
    globalThis.moduleLoadersReleasedCodeNextGate = undefined;
    const h = await load(lazyTLA);
    assert(g.marker === 7 && h.marker === 7 && g !== h, "both ran");
    checkIsolated(g, h, "deleted while suspended");
}

test().then(() => { }, (e) => { print("FAIL: " + e + "\n" + e.stack); $vm.abort(); });
