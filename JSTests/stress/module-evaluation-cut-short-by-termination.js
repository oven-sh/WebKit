//@ runDefault("--useDollarVM=1", "--useImportDefer=1")

// An embedder can stop script with a termination and then withdraw it: VM::addTerminationDeadline() and
// VM::cancelTermination(), here through $vm.callWithTimeLimit (what node:vm's `timeout` is built on). A module whose
// evaluation the termination cut short is over, like a module that threw. Its error is null, never the termination's
// own value: for a later import(), for a module that imports it, and for a read of its deferred namespace.

const resources = "./resources/module-evaluation-cut-short-by-termination/";
// A module body of this test logs its name. The one to cut short then waits, off the CPU, for `wait` milliseconds:
// longer than the limit. The termination wakes the wait and is thrown from it. A wait that ends by itself logs that.
const cutShort = globalThis.cutShort = { log: [], wait: 0 };
const failures = [];

function describe(value) {
    if (value === null)
        return "null";
    return typeof value === "string" ? `the string ${JSON.stringify(value)}` : `${typeof value} ${String(value)}`;
}

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        failures.push(`${what}: expected ${expected}, got ${actual}`);
}

function timesOut(fn, ms) {
    try {
        $vm.callWithTimeLimit(fn, ms);
    } catch (e) {
        if (e instanceof RangeError && e.message == "timed out")
            return true;
        throw e;
    }
    return false;
}

// The shell's loader reads a file inside the job that asks for it, so one drain settles an import().
function outcome(promise) {
    let result = "pending";
    promise.then(() => { result = "fulfilled"; }, (error) => { result = "rejected with " + describe(error); });
    drainMicrotasks();
    return result;
}

function thrownBy(fn) {
    try {
        fn();
    } catch (e) {
        return "threw " + describe(e);
    }
    return "returned";
}

// `scenario(directory)` gives `start`, which runs under a limit, and `check`, which runs after it. `logWhenCutShort`
// is the log of a limit that passed inside the wait of the body to cut short. On a slow or a busy machine a limit can
// pass before that body starts, or after its wait has ended. Such a turn proves nothing, and the next turn has a longer
// limit and new module records: a specifier with one more "/" is another URL of the same file. A wait that ends in
// two turns is not a slow machine: the limit does not stop a body that waits.
function cutShortInside(what, logWhenCutShort, scenario) {
    let waitsThatEnded = 0;
    for (let turn = 0, limit = 10; turn < 7 && waitsThatEnded < 2; ++turn, limit *= 4) {
        const { start, check } = scenario(resources + "/".repeat(turn));
        cutShort.log.length = 0;
        cutShort.wait = limit + 1000;
        const timedOut = timesOut(start, limit);
        // A body that starts after the limited call does not wait.
        cutShort.wait = 0;
        if (cutShort.log.join() != logWhenCutShort) {
            if (cutShort.log.some((entry) => entry.endsWith(": the wait ended")))
                ++waitsThatEnded;
            // The jobs that this turn left in the queue run to their end here, and not under the next limit.
            drainMicrotasks();
            continue;
        }
        shouldBe(timedOut, true, `${what}: the limit passed`);
        shouldBe($vm.hasPendingTermination(), false, `${what}: nothing is pending after the withdrawal`);
        check();
        return;
    }
    failures.push(`${what}: no limit passed inside the wait of the module body`);
}

// 1. An import() that a job evaluates inside the limited call.
cutShortInside("import()", "spin", (directory) => {
    let inFlight = "pending";
    return {
        start() {
            import(directory + "spin.js").then(() => { inFlight = "fulfilled"; }, (error) => { inFlight = "rejected with " + describe(error); });
            drainMicrotasks();
        },
        check() {
            shouldBe(outcome(import(directory + "spin.js")), "rejected with null", "import() of the module that was cut short");
            shouldBe(outcome(import(directory + "spin.js")), "rejected with null", "import() of it again");
            shouldBe(outcome(import(directory + "spin-importer.js")), "rejected with null", "import() of a module that imports it");
            // The termination took the job that was evaluating the module, and the jobs behind it in the queue.
            shouldBe(inFlight, "pending", "the import() whose job the limit stopped");
            shouldBe(cutShort.log.join(), "spin", "import(): no body ran afterwards");
        },
    };
});

// 2. A graph: the root imports a module that finishes, the module that is cut short, and a module that is not reached.
cutShortInside("graph", "graph-leaf,graph-spin", (directory) => ({
    start() {
        import(directory + "graph-root.js").then(() => { }, () => { });
        drainMicrotasks();
    },
    check() {
        shouldBe(outcome(import(directory + "graph-root.js")), "rejected with null", "graph: the root, on the stack of the walk");
        shouldBe(outcome(import(directory + "graph-spin.js")), "rejected with null", "graph: the module that was running");
        shouldBe(outcome(import(directory + "graph-leaf.js")), "fulfilled", "graph: a module that had finished");
        shouldBe(outcome(import(directory + "graph-never.js")), "fulfilled", "graph: a module that the walk did not reach");
        shouldBe(cutShort.log.join(), "graph-leaf,graph-spin,graph-never", "graph: only the module not reached runs afterwards");
    },
}));

// 3. The first read of a deferred namespace inside the limited call: EvaluateModuleSync.
cutShortInside("deferred namespace", "deferred-spin", (directory) => {
    // Loaded and linked, not evaluated.
    let namespace;
    import.defer(directory + "deferred-spin.js").then((value) => { namespace = value; });
    drainMicrotasks();
    return {
        start() {
            namespace.before;
        },
        check() {
            shouldBe(thrownBy(() => namespace.before), "threw null", "deferred namespace: a second read");
            shouldBe(thrownBy(() => namespace.after), "threw null", "deferred namespace: a read of a binding the body did not reach");
            shouldBe(outcome(import(directory + "deferred-spin.js")), "rejected with null", "deferred namespace: import() of the module");
            shouldBe(cutShort.log.join(), "deferred-spin", "deferred namespace: no body ran afterwards");
        },
    };
});

// 4. Two walks on the stack: a module body reads a deferred namespace, and that module is cut short.
cutShortInside("nested", "nested-outer,nested-inner", (directory) => ({
    start() {
        import(directory + "nested-outer.js").then(() => { }, () => { });
        drainMicrotasks();
    },
    check() {
        shouldBe(outcome(import(directory + "nested-inner.js")), "rejected with null", "nested: the inner module");
        shouldBe(outcome(import(directory + "nested-outer.js")), "rejected with null", "nested: the outer module");
        shouldBe(cutShort.log.join(), "nested-outer,nested-inner", "nested: no body ran afterwards");
    },
}));

// 5. A module with top-level await that is cut short before its first await.
cutShortInside("top-level await", "tla-spin", (directory) => ({
    start() {
        import(directory + "tla-spin.js").then(() => { }, () => { });
        drainMicrotasks();
    },
    check() {
        shouldBe(outcome(import(directory + "tla-spin.js")), "rejected with null", "top-level await: import() of the module");
        shouldBe(cutShort.log.join(), "tla-spin", "top-level await: no body ran afterwards");
    },
}));

if (failures.length)
    throw new Error(`${failures.length} failed:\n` + failures.join("\n"));
