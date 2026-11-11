// import() gives the program a promise of its own, which the loader's promise settles (globalFuncImportModule(), under
// USE(BUN_JSC_ADDITIONS)). No program has the loader's promise, and the reaction that settles the program's promise is added
// to it at the call. resolve() would look up `then` on it and queue a job to add the reaction. So:
// - import() settles at the same place among the other jobs whether or not Promise.prototype.then or
//   Promise[Symbol.species] is replaced, and import() asks neither of them;
// - a namespace that exports `then` is resolved as a thenable;
// - a rejection that nothing handles is reported once.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + ", got " + actual);
}

const resources = "./resources/bun-dynamic-import-links-its-promise-at-the-call/";

// How many jobs of a chain of `await null` run before the promise of start() settles, and what it settles with.
async function settles(start) {
    const log = [];
    const chain = (async () => {
        for (let turn = 1; turn <= 40; turn++) {
            await null;
            log.push(turn);
        }
    })();
    let settledWith;
    start().then(
        value => { log.push("settled"); settledWith = typeof value === "string" ? value : value.name; },
        error => { log.push("settled"); settledWith = String(error); });
    await chain;
    return log.indexOf("settled") + ": " + settledWith;
}

async function rows(directory) {
    const from = resources + directory + "/";
    await import(from + "evaluated.js");
    return JSON.stringify({
        evaluated: await settles(() => import(from + "evaluated.js")),
        new: await settles(() => import(from + "new.js")),
        awaits: await settles(() => import(from + "awaits.js")),
        throws: await settles(() => import(from + "throws.js")),
        threwBefore: await settles(() => import(from + "throws.js")),
        missing: (await settles(() => import(from + "missing.js"))).replace(/: .*/, ": rejected"),
        thenable: await settles(() => import(from + "thenable.js")),
        thenableAgain: await settles(() => import(from + "thenable.js")),
    });
}
// settles() itself calls then() once for each of the 8 rows. import() calls it for none.
const thensOfRows = 8;

(async function () {
    const original = await rows("original");
    const row = JSON.parse(original);
    shouldBe(row.evaluated.replace(/^\d+/, ""), ": evaluated", "an evaluated module");
    shouldBe(row.thenable.replace(/^\d+/, ""), ": what then() resolved with", "a namespace that exports then");
    shouldBe(row.thenableAgain.replace(/^\d+/, ""), ": what then() resolved with", "a namespace that exports then, evaluated before");
    shouldBe(row.throws.replace(/^\d+/, ""), ": Error: thrown by the module", "a module that throws");
    shouldBe(row.threwBefore.replace(/^\d+/, ""), ": Error: thrown by the module", "a module that threw before");

    let reported = 0;
    setUnhandledRejectionCallback(() => { reported++; });
    import(resources + "original/missing.js");
    for (let turn = 0; turn < 40; turn++)
        await null;
    drainMicrotasks();
    shouldBe(reported, 1, "reports of a rejected import() that nothing handles");
    import(resources + "original/missing.js").catch(() => { });
    for (let turn = 0; turn < 40; turn++)
        await null;
    drainMicrotasks();
    shouldBe(reported, 1, "reports after a rejected import() that a catch() handles");
    setUnhandledRejectionCallback(() => { $vm.abort(); });

    let speciesReads = 0;
    Object.defineProperty(Promise, Symbol.species, { get() { speciesReads++; return this; } });
    shouldBe(await rows("replaced-species"), original, "with Promise[Symbol.species] replaced");
    shouldBe(speciesReads, thensOfRows, "reads of the replaced Promise[Symbol.species]");

    const then = Promise.prototype.then;
    let thenCalls = 0;
    Promise.prototype.then = function (onFulfilled, onRejected) {
        thenCalls++;
        return then.call(this, onFulfilled, onRejected);
    };
    shouldBe(await rows("replaced-then"), original, "with Promise.prototype.then replaced");
    Promise.prototype.then = then;
    shouldBe(thenCalls, thensOfRows, "calls of the replaced Promise.prototype.then");
}()).then(() => { }, error => {
    print(error);
    $vm.abort();
});
