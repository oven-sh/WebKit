//@ runDefault("--useDollarVM=1", "--useJIT=0")

// As module-load-cut-short-by-termination.js, but the call that is cut is a synchronous load
// (JSModuleLoader::loadModuleSync(), what `require()` of an ES module is in Bun). Its jobs run in the synchronous queue
// of the loader, and a termination ends the call with jobs left in that queue.

load("./resources/module-load-cut-short-by-termination/walk.js");

function beginSynchronousLoad(loader, name, round) {
    round.value = loadSynchronously(loader, name).value;
}

// The same while an import() of the root is in flight: the synchronous load takes over the steps of that load.
function beginSynchronousLoadBehindImport(loader, name, round) {
    round.first = importFile(loader, name, "root.js");
    round.value = loadSynchronously(loader, name).value;
}

function checkSynchronousLoad(what, graph, round, wasCut) {
    if (wasCut) {
        shouldBe(round.value, undefined, `${what}: the call returned`);
        return;
    }
    if (graph.hasTopLevelAwait) {
        // The load cannot finish inside the call.
        if (!(round.threw instanceof TypeError))
            throw new Error(`${what}: the synchronous load of a module that waits threw ${round.threw}`);
        return;
    }
    shouldBe(round.threw, undefined, `${what}: the synchronous load threw`);
    shouldBe(round.value, graph.values["root.js"], `${what}: the value of the synchronous load`);
}

walk("two", { begin: beginSynchronousLoad, check: checkSynchronousLoad });
walk("two", { begin: beginSynchronousLoad, check: checkSynchronousLoad, cutAgain: true });
walk("two", { begin: beginSynchronousLoadBehindImport, check: checkSynchronousLoad });
walk("chain", { begin: beginSynchronousLoad, check: checkSynchronousLoad });
walk("diamond", { begin: beginSynchronousLoad, check: checkSynchronousLoad });
walk("cycle", { begin: beginSynchronousLoad, check: checkSynchronousLoad });
walk("await", { begin: beginSynchronousLoad, check: checkSynchronousLoad });
