// Shared by module-load-cut-short-by-termination.js and module-load-cut-short-by-termination-sync.js.
//
// walk() takes a termination at the n-th trap check of one call that loads a module graph, for every n
// ($vm.callWithTerminationAtTrapCheck()), and then loads the same modules again through the same loader. Each round
// has a module loader of its own ($vm.createModuleLoader()), so each round loads the files afresh.

// The graphs are in directories next to this file. $vm.moduleLoaderImport() resolves a path from its caller.
const directory = "./";
const terminationValue = "JavaScript execution terminated.";
// Each module of the graphs counts the runs of its body here, under its file name.
globalThis.bodies = new Map;

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

const graphs = {
    two: { modules: 2, values: { "root.js": 41, "dep.js": 40 } },
    chain: { modules: 3, values: { "root.js": 42, "mid.js": 41, "dep.js": 40 } },
    diamond: { modules: 4, values: { "root.js": 83, "dep.js": 40 } },
    cycle: { modules: 2, values: { "root.js": 41, "dep.js": 40 } },
    // dep.js has a top-level await.
    await: { modules: 2, values: { "root.js": 41, "dep.js": 40 }, hasTopLevelAwait: true },
};

// import() of a file of a graph through `loader`, and what became of it.
function importFile(loader, graph, file) {
    const load = { file, state: "pending", value: undefined };
    $vm.moduleLoaderImport(loader, `${directory}${graph}/${file}`).then(namespace => {
        load.value = namespace.value;
        load.state = "fulfilled";
    }, error => {
        load.value = error;
        load.state = "rejected";
    });
    return load;
}

// JSModuleLoader::loadModuleSync() for the root of a graph.
function loadSynchronously(loader, graph) {
    return $vm.moduleLoaderLoadSync(loader, `${directory}${graph}/root.js`);
}

// A module that the termination stopped in its evaluation keeps that as its evaluation error, and a later import() of it
// rejects with the value (null once a stopped module records null). The evaluation is not the subject of these tests.
function isStoppedInEvaluation(load) {
    return load.state == "rejected" && (load.value === null || load.value === terminationValue);
}

// "fulfilled": each load has its module. "stopped": a module was stopped in its evaluation. "pending": a load waits.
function outcomeOf(what, graph, loads) {
    for (const [file, count] of bodies) {
        if (count > 1)
            throw new Error(`${what}: the body of ${file} ran ${count} times`);
    }
    let outcome = "fulfilled";
    for (const load of loads) {
        if (load.state == "pending")
            return "pending";
        if (isStoppedInEvaluation(load))
            outcome = "stopped";
        else {
            shouldBe(load.state, "fulfilled", `${what}: the later import() of ${load.file} (${load.value})`);
            shouldBe(load.value, graph.values[load.file], `${what}: the value of ${load.file}`);
        }
    }
    if (outcome == "fulfilled")
        shouldBe(bodies.size, graph.modules, `${what}: the modules that have run`);
    return outcome;
}

// What a caller does after it has caught the time limit: it loads the modules again.
const laterLoads = [
    // The root, and when that has settled a module that the root imports.
    (loader, graph, leaf) => {
        const root = importFile(loader, graph, "root.js");
        drainMicrotasks();
        const dep = importFile(loader, graph, leaf);
        drainMicrotasks();
        return [root, dep];
    },
    // The module that the root imports and the root, in the turn in which the termination was withdrawn.
    (loader, graph, leaf) => {
        const dep = importFile(loader, graph, leaf);
        const root = importFile(loader, graph, "root.js");
        drainMicrotasks();
        return [root, dep];
    },
];

// `begin(loader, name, round)` is the call that is cut. It loads root.js of the graph `name`, and may record in `round`
// what it has. `check(what, graph, round, wasCut)` looks at what the call left.
function walk(name, { begin, check = () => { }, leaf = "dep.js", cutAgain = false }) {
    const graph = graphs[name];
    // A function has more trap checks while its inline caches are empty. After some rounds the n-th check is a fixed place.
    for (let i = 0; i < 10; ++i) {
        for (const later of laterLoads) {
            const loader = $vm.createModuleLoader();
            try {
                begin(loader, name, { });
            } catch { }
            later(loader, name, leaf);
        }
    }

    for (const [order, later] of laterLoads.entries()) {
        // The cuts of a walk come in this order: in the load and the link (every later load has its module), in the
        // evaluation (a module is stopped), and behind the evaluation (every later load has its module again).
        let phase = "load";
        for (let n = 1; ; ++n) {
            if (n > 100000)
                throw new Error(`${name}: the call has no end`);
            const what = `${name}, trap check ${n}, later loads ${order}`;
            const loader = $vm.createModuleLoader();
            const round = { };
            bodies.clear();
            let threw;
            const wasCut = $vm.callWithTerminationAtTrapCheck(() => {
                try {
                    begin(loader, name, round);
                } catch (error) {
                    threw = error;
                }
            }, n);
            shouldBe($vm.hasPendingTermination(), false, `${what}: a termination is still pending`);
            round.threw = threw;
            check(what, graph, round, wasCut);

            if (cutAgain && wasCut) {
                // The load that comes next is cut too, at the same count.
                $vm.callWithTerminationAtTrapCheck(() => { later(loader, name, leaf); }, n);
                shouldBe($vm.hasPendingTermination(), false, `${what}: a termination is still pending after the second cut`);
            }

            const outcome = outcomeOf(what, graph, later(loader, name, leaf));
            if (outcome == "pending" && !(phase == "evaluation" && graph.hasTopLevelAwait))
                throw new Error(`${what}: a later import() is still pending`);
            if (cutAgain) {
                // The second cut is in another part of its load than the first, so the parts do not come in order.
                phase = wasCut ? phase : "after";
            } else if (outcome == "stopped") {
                if (phase == "after")
                    throw new Error(`${what}: a module is stopped, and with a smaller count it was not`);
                phase = "evaluation";
            } else if (outcome == "fulfilled" && phase == "evaluation")
                phase = "after";

            if (!wasCut) {
                shouldBe(outcome, "fulfilled", `${what}: the later loads of a call that was not cut`);
                break;
            }
        }
        // No walk passes without a cut in each part.
        shouldBe(phase, "after", `${name}, later loads ${order}: the last part that the walk reached`);
    }
}
