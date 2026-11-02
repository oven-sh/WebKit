//@ runDefault("--useDollarVM=1", "--useJIT=0")

// A termination can begin inside the link of a module, behind the step that sets the environment of the module:
// in CyclicModuleRecord::initializeEnvironment(), and in SyntheticModuleRecord::initializeExports() for a module
// without text whose exports are made at the link. The module is then linked again by the next load, and that link
// sets an environment again. So the cut link has to leave none: ASSERT(!m_moduleEnvironment) in an assertion build.
//
// Each walk takes a termination at each trap check of an import() in turn ($vm.callWithTerminationAtTrapCheck()) and
// imports the module again. A cut inside the load is not the subject here: there the later import() can stay pending.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

const terminationValue = "JavaScript execution terminated.";
// The modules with text add their names here when they run.
globalThis.loadSyncLog = [];
// The number of modules that the loader asked the shell for.
let fetched = 0;
setModuleLoaderFetchHook(key => {
    ++fetched;
    if (key.endsWith("/imports-object.js"))
        return `import { a, b, c } from "./object.js"; globalThis.loadSyncLog.push("imports-object"); export const value = a + b + c;`;
    // A module without text. The shell reads its exports when the graph is about to be linked.
    if (key.endsWith("/object.js")) {
        return {
            get a() {
                exportsWereRead = true;
                return 1;
            },
            b: 2,
            c: 38,
        };
    }
});
// Whether the exports of the module without text have been read.
let exportsWereRead = false;

function importRoot(loader, root) {
    const load = { state: "pending", value: undefined };
    $vm.moduleLoaderImport(loader, root).then(namespace => {
        load.value = namespace.value;
        load.state = "fulfilled";
    }, error => {
        load.value = error;
        load.state = "rejected";
    });
    drainMicrotasks();
    return load;
}

// `inTheLink()` tells whether a cut import() that has loaded its graph and has run no module was cut in the link, where
// the test can tell.
function walk(what, root, modulesWithText, inTheLink = () => true) {
    // A function has more trap checks while its inline caches are empty, so the walk imports the graph a few times first.
    for (let i = 0; i < 10; ++i)
        importRoot($vm.createModuleLoader(), root);

    let linkedAgain = 0;
    for (let n = 1; ; ++n) {
        if (n > 100000)
            throw new Error(`${what}: the import() has no end`);
        const loader = $vm.createModuleLoader();
        loadSyncLog.length = 0;
        exportsWereRead = false;
        const wasCut = $vm.callWithTerminationAtTrapCheck(() => { importRoot(loader, root); }, n);
        shouldBe($vm.hasPendingTermination(), false, `${what}, trap check ${n}: a termination is still pending`);
        const ranBeforeTheCut = loadSyncLog.length;
        const wasCutInTheLink = inTheLink();

        loadSyncLog.length = 0;
        fetched = 0;
        const later = importRoot(loader, root);
        if (later.state == "fulfilled") {
            shouldBe(later.value, 41, `${what}, trap check ${n}: the value of the later import()`);
            // The later import() fetched nothing and ran each module. So the cut import() had loaded the graph and
            // had run none of it: the termination began in the link or next to it, and the later import() linked
            // the graph.
            if (wasCut && !ranBeforeTheCut && wasCutInTheLink && !fetched && loadSyncLog.length == modulesWithText)
                ++linkedAgain;
        } else if (later.state == "rejected") {
            // A module that the termination stopped in its evaluation keeps that as its error.
            if (later.value !== null && later.value !== terminationValue)
                throw new Error(`${what}, trap check ${n}: the later import() rejected with ${later.value}`);
        }
        if (!wasCut) {
            shouldBe(later.state, "fulfilled", `${what}, trap check ${n}: the later import() of a call that was not cut`);
            break;
        }
    }
    if (!linkedAgain)
        throw new Error(`${what}: no later import() linked a graph again whose link the termination had cut`);
}

walk("two modules", "./resources/module-loader-load-sync/root.js", 2);
// The exports of the module without text are read in the link, before SyntheticModuleRecord::initializeExports().
walk("a module without text", "./module-link-cut-short-by-termination/imports-object.js", 1, () => exportsWereRead);
