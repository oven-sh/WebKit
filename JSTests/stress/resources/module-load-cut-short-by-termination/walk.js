// Shared by the module-load-cut-short-by-termination*.js tests.
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
    // A module of these two waits in its evaluation: dep.js of `await` at a top-level await, and root.js of `aimp`
    // for its own import() of dep.js. Where a termination stops what the module waits for (the evaluation of a
    // module, or a job of that import()), nothing resumes the module, and a later import() of it waits too. That is
    // the evaluation of a module, which these tests do not cover: they allow it in that part of a walk only.
    await: { modules: 2, values: { "root.js": 41, "dep.js": 40 }, waitsInEvaluation: true },
    aimp: { modules: 2, values: { "root.js": 41, "dep.js": 40 }, waitsInEvaluation: true },
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

// The call that is cut: an import() of the root, and the checkpoint that runs its jobs.
function beginImport(loader, name, round) {
    round.imports = [importFile(loader, name, "root.js")];
    drainMicrotasks();
}

// An import() that was in flight when the call was cut has no answer, or its load has gone on and has answered it
// with its module. It is never answered with something else. `settled` is false right behind the cut, and true when
// the later loads have run.
function checkImports(what, graph, round, wasCut, settled) {
    if (!round.imports) {
        shouldBe(wasCut, true, `${what}: the call returned`);
        return;
    }
    for (const load of round.imports) {
        if (!wasCut && !round.mayStayPending)
            shouldBe(load.state, "fulfilled", `${what}: an import() of a call that was not cut`);
        if (load.state == "fulfilled")
            shouldBe(load.value, graph.values[load.file], `${what}: the value of an import() that was in flight${settled ? ", in the end" : ""}`);
        else if (load.state == "rejected" && !isStoppedInEvaluation(load))
            throw new Error(`${what}: an import() that was in flight rejected with ${load.value}${settled ? ", in the end" : ""}`);
    }
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
    // A synchronous load of the root. Where jobs of the load that was cut are in the queue of the VM, it cannot
    // finish inside the call and says so. An import() has the module then.
    (loader, graph, leaf) => {
        let root = { file: "root.js", state: "fulfilled", value: undefined };
        try {
            root.value = loadSynchronously(loader, graph).value;
        } catch (error) {
            if (isStoppedInEvaluation({ state: "rejected", value: error }))
                root = { file: "root.js", state: "rejected", value: error };
            else {
                if (!(error instanceof TypeError))
                    throw error;
                root = importFile(loader, graph, "root.js");
            }
        }
        drainMicrotasks();
        const dep = importFile(loader, graph, leaf);
        drainMicrotasks();
        return [root, dep];
    },
];

// `begin(loader, name, round)` is the call that is cut. It loads root.js of the graph `name`, and records in `round`
// what it has. `check(what, graph, round, wasCut, settled)` looks at what the call left, behind the cut and in the end.
// `secondCuts(n)` gives the counts at which the loads that come next are cut too: each is a round of its own.
// `synchronousLaterLoad` adds the walk whose later load of the root is a synchronous one.
function walk(name, { begin, check = () => { }, leaf = "dep.js", secondCuts = null, synchronousLaterLoad = false }) {
    const graph = graphs[name];
    const orders = laterLoads.slice(0, synchronousLaterLoad ? 3 : 2);
    // A function has more trap checks while its inline caches are empty. After some rounds the n-th check is a fixed place.
    for (let i = 0; i < 10; ++i) {
        for (const later of orders) {
            const loader = $vm.createModuleLoader();
            try {
                begin(loader, name, { });
            } catch { }
            later(loader, name, leaf);
        }
    }

    for (const [order, later] of orders.entries()) {
        // The cuts of a walk come in this order: in the load and the link (every later load has its module), in the
        // evaluation (a module is stopped), and behind the evaluation (every later load has its module again). In a
        // graph with a module that waits in its evaluation, the evaluation goes on to the end of the call.
        let phase = "load";
        for (let n = 1; ; ++n) {
            if (n > 100000)
                throw new Error(`${name}: the call has no end`);
            let wasCut;
            for (const second of secondCuts ? secondCuts(n) : [0]) {
                const what = `${name}, trap check ${n}${second ? ` and then ${second}` : ""}, later loads ${order}`;
                const loader = $vm.createModuleLoader();
                const round = { };
                bodies.clear();
                let threw;
                wasCut = $vm.callWithTerminationAtTrapCheck(() => {
                    try {
                        begin(loader, name, round);
                    } catch (error) {
                        threw = error;
                    }
                }, n);
                shouldBe($vm.hasPendingTermination(), false, `${what}: a termination is still pending`);
                round.threw = threw;
                check(what, graph, round, wasCut, false);

                if (second && wasCut) {
                    // Two import() of the root are in flight when the second termination comes.
                    $vm.callWithTerminationAtTrapCheck(() => {
                        importFile(loader, name, "root.js");
                        importFile(loader, name, "root.js");
                        drainMicrotasks();
                    }, second);
                    shouldBe($vm.hasPendingTermination(), false, `${what}: a termination is still pending after the second cut`);
                }

                const outcome = outcomeOf(what, graph, later(loader, name, leaf));
                check(what, graph, round, wasCut, true);
                if (outcome == "pending" && !(phase == "evaluation" && graph.waitsInEvaluation))
                    throw new Error(`${what}: a later import() is still pending`);
                if (second) {
                    // The second cut is in another part of its load than the first, so the parts do not come in order.
                    phase = wasCut ? phase : "after";
                } else if (outcome == "stopped") {
                    if (phase == "after")
                        throw new Error(`${what}: a module is stopped, and with a smaller count it was not`);
                    phase = "evaluation";
                } else if (outcome == "fulfilled" && phase == "evaluation" && !graph.waitsInEvaluation)
                    phase = "after";

                if (!wasCut) {
                    shouldBe(outcome, "fulfilled", `${what}: the later loads of a call that was not cut`);
                    break;
                }
            }
            if (!wasCut)
                break;
        }
        // No walk passes without a cut in each part.
        shouldBe(phase, graph.waitsInEvaluation ? "evaluation" : "after", `${name}, later loads ${order}: the last part that the walk reached`);
    }
}
