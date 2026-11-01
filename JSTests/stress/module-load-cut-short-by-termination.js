//@ runDefault("--useDollarVM=1", "--useJIT=0")

// A termination can end a module load at any exception check, and an embedder can then withdraw it
// (VM::cancelTermination(): node:vm's `timeout` in Bun). The loader goes on after that, so it must hold nothing that
// makes a later load of the same modules wait for ever:
// - a registry entry whose module promise or load promise nothing settles, because the step that settles it was cut,
// - a graph loading state that waits for a request which was never made,
// - a load job that was only queued when the checkpoint ended, and that the queue dropped.
//
// Here the call that is cut is an import() and the checkpoint that runs its jobs.
// module-load-cut-short-by-termination-graphs.js does the same for more graphs,
// module-load-cut-short-by-termination-sync.js for a synchronous load, and
// module-load-cut-short-by-termination-nested.js for a load inside the fetch hook of another load.

load("./resources/module-load-cut-short-by-termination/walk.js");

// The jobs of the load share the queue with jobs of script, so a job of script is cut with jobs of the load behind it.
// The jobs of script that the cut checkpoint drops stay dropped: only the jobs of the loader go on.
let roundsWithDroppedJobs = 0;
function beginImportAmongJobsOfScript(loader, name, round) {
    round.ticks = 0;
    round.imports = [importFile(loader, name, "root.js")];
    let job = Promise.resolve();
    for (let i = 0; i < 6; ++i)
        job = job.then(() => { ++round.ticks; });
    drainMicrotasks();
}
function checkImportsAmongJobsOfScript(what, graph, round, wasCut, settled) {
    checkImports(what, graph, round, wasCut, settled);
    if (!settled || !round.imports)
        return;
    if (!wasCut)
        shouldBe(round.ticks, 6, `${what}: the jobs of script that ran`);
    else if (round.ticks < 6)
        ++roundsWithDroppedJobs;
}

// Two import() of the root are in flight when the termination comes.
function beginTwoImports(loader, name, round) {
    round.imports = [importFile(loader, name, "root.js"), importFile(loader, name, "root.js")];
    drainMicrotasks();
}

walk("two", { begin: beginImport, check: checkImports, synchronousLaterLoad: true });
walk("two", { begin: beginImport, check: checkImports, secondCuts: n => [n, n + 5, 2 * n] });
walk("two", { begin: beginTwoImports, check: checkImports });
walk("two", { begin: beginImportAmongJobsOfScript, check: checkImportsAmongJobsOfScript });
if (!roundsWithDroppedJobs)
    throw new Error("no cut checkpoint dropped a job of script");
walk("chain", { begin: beginImport, check: checkImports });

// A module of the graph cannot be fetched, so the load of the root fails. A later load of the root is answered with
// that error, and is not left to wait. The error of one load is not kept in a registry entry that got no fetch: two
// later loads of the missing module each fail with an error of their own.
{
    const failed = load => load.state == "rejected" && load.value instanceof Error && load.value.message.includes("Could not open file");
    const round = n => {
        const loader = $vm.createModuleLoader();
        const what = `failing, trap check ${n}`;
        const begin = () => {
            importFile(loader, "failing", "root.js");
            drainMicrotasks();
        };
        const wasCut = n ? $vm.callWithTerminationAtTrapCheck(begin, n) : (begin(), false);
        shouldBe($vm.hasPendingTermination(), false, `${what}: a termination is still pending`);
        // The missing module first: a cut behind the fetch hook has left its entry without a fetch.
        const missing = importFile(loader, "failing", "missing.js");
        drainMicrotasks();
        const missingAgain = importFile(loader, "failing", "missing.js");
        drainMicrotasks();
        const root = importFile(loader, "failing", "root.js");
        drainMicrotasks();
        for (const load of [root, missing, missingAgain]) {
            if (!failed(load))
                throw new Error(`${what}: the later import() of ${load.file} is ${load.state} (${load.value})`);
        }
        if (missing.value === missingAgain.value)
            throw new Error(`${what}: two loads of the missing module failed with one error object`);
        return wasCut;
    };
    for (let i = 0; i < 10; ++i)
        round(0);
    for (let n = 1; round(n); ++n) {
        if (n > 100000)
            throw new Error("failing: the call has no end");
    }
}
