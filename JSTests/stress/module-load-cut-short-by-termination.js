//@ runDefault("--useDollarVM=1")

// A termination can end a module load at any exception check, and an embedder can then withdraw it
// (VM::cancelTermination(): node:vm's `timeout`, $vm.callWithTimeLimit() here). The loader goes on after that,
// so it must hold nothing that makes a later load of the same modules wait for ever:
// - a registry entry whose fetch was never started (ASSERT(mapEntry->status() != New) in hostLoadImportedModule),
// - an entry whose module or load promise nothing settles, because the step that settles it was the one cut,
// - a load job that was only queued when the checkpoint ended, and that the queue dropped.
//
// The first half takes the termination at the n-th trap check of a load, for every n ($vm.callWithTerminationAtTrapCheck).
// The second half lets a time limit end a job of script while the jobs of a load wait behind it in the queue.

const directory = "./resources/module-load-cut-short-by-termination/";
const terminationValue = "JavaScript execution terminated.";
globalThis.bodies = new Map;

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

// A directory name with more slashes is another URL of the same files, so each copy has module records of its own.
let copies = 0;
function nextCopy(graph) {
    return directory + graph + "/".repeat(++copies);
}

function load(specifier) {
    const load = { specifier, state: "pending", value: undefined };
    import(specifier).then(namespace => {
        load.state = "fulfilled";
        load.value = namespace.value;
    }, error => {
        load.state = "rejected";
        load.value = error;
    });
    return load;
}

// A module that the termination stopped in its evaluation keeps that as its error, and a later import() of it rejects
// with the value (null once a stopped module records null). That is not the subject here; the load phase is.
function isStoppedInEvaluation(load) {
    return load.state == "rejected" && (load.value === null || load.value === terminationValue);
}

function checkLaterLoads(what, loads, expected) {
    let stopped = false;
    for (const [name, load] of Object.entries(loads)) {
        if (load.state == "pending")
            throw new Error(`${what}: the later import() of ${name} is still pending`);
        if (isStoppedInEvaluation(load)) {
            stopped = true;
            continue;
        }
        shouldBe(load.state, "fulfilled", `${what}: the later import() of ${name} (${load.value})`);
        shouldBe(load.value, expected[name], `${what}: the value of ${name}`);
    }
    for (const [file, count] of bodies)
        shouldBe(count, 1, `${what}: runs of the body of ${file}`);
    bodies.clear();
    return !stopped;
}

const orders = [
    // The root, and when that has settled the leaf.
    (copy, leaf) => {
        const root = load(copy + "root.js");
        drainMicrotasks();
        const dep = load(copy + leaf);
        drainMicrotasks();
        return { root, dep };
    },
    // The leaf and the root in the turn in which the termination was withdrawn.
    (copy, leaf) => {
        const dep = load(copy + leaf);
        const root = load(copy + "root.js");
        drainMicrotasks();
        return { root, dep };
    },
];

function sweep(graph, expected, minimumCuts) {
    let cuts = 0;
    let loadedAgain = 0;
    for (let n = 1; ; ++n) {
        let wasCut = false;
        for (const [index, later] of orders.entries()) {
            const copy = nextCopy(graph);
            const what = `${graph}, trap check ${n}, order ${index}`;
            let first;
            wasCut = $vm.callWithTerminationAtTrapCheck(() => {
                first = load(copy + "root.js");
                drainMicrotasks();
            }, n);
            shouldBe($vm.hasPendingTermination(), false, `${what}: a termination is still pending`);
            if (!wasCut) {
                shouldBe(first.state, "fulfilled", `${what}: the load that was not cut`);
                shouldBe(first.value, expected.root, `${what}: the value of the load that was not cut`);
            }
            if (checkLaterLoads(what, later(copy, "dep.js"), expected))
                ++loadedAgain;
        }
        if (!wasCut)
            break;
        ++cuts;
        if (n > 100000)
            throw new Error(`${graph}: the load has no end`);
    }
    // Neither loop can pass by cutting nothing.
    if (cuts < minimumCuts || loadedAgain < minimumCuts)
        throw new Error(`${graph}: ${cuts} trap checks were cut and ${loadedAgain} later loads fulfilled, expected at least ${minimumCuts} of each`);
}

sweep("two", { root: 41, dep: 40 }, 40);
sweep("chain", { root: 42, dep: 40 }, 60);
sweep("diamond", { root: 83, dep: 40 }, 80);

// The second half. A load is in flight when a job of script that never ends has its turn, `turns` jobs into the
// checkpoint, and a time limit ends that job. The jobs of the load are behind it in the queue, or not queued yet.
let cutJobs = 0;
for (let turns = 0; turns < 24; ++turns) {
    const copy = nextCopy("chain");
    const what = `a job of script cut after ${turns} turns`;
    const first = load(copy + "root.js");
    let job = Promise.resolve();
    for (let i = 0; i < turns; ++i)
        job = job.then(() => { });
    job.then(() => {
        if (first.state == "pending") {
            for (;;) { }
        }
    });

    try {
        $vm.callWithTimeLimit(drainMicrotasks, 50);
    } catch (error) {
        if (!(error instanceof RangeError) || error.message != "timed out")
            throw error;
        ++cutJobs;
    }
    shouldBe($vm.hasPendingTermination(), false, `${what}: a termination is still pending`);

    checkLaterLoads(what, orders[turns % orders.length](copy, "dep.js"), { root: 42, dep: 40 });
}
if (cutJobs < 8)
    throw new Error(`${cutJobs} jobs of script were cut, expected at least 8`);
