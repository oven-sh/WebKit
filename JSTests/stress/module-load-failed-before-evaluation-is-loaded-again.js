// The bytecode-cache mode runs a test twice and the second time takes all code from the cache of the first. The modules
// this test writes have new names on every run, so they are never in that cache.
//@ $skipModes << "bytecode-cache".to_sym

// Bun: this fork's test. A load that fails before its module ran does not stay the answer for the module's key: the
// next load fetches again. That holds for the module whose fetch or parse failed, and for every module that imports it.
// A module that threw while it was evaluated, or that did not link, stays as it is.
//
// The test writes the modules it imports into resources/, under names of their own for every run.

function shouldBe(actual, expected, what)
{
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected} but got ${actual}`);
}

// writeFile() takes a path as the shell was given this file's; import() resolves against this file.
const directory = /@(.*?)[^\/\\]*:\d+:\d+$/m.exec(new Error().stack)[1];
const run = `${Date.now()}-${Math.floor(Math.random() * 1e9)}`;
const name = label => `resources/module-load-failed-before-evaluation-${run}-${label}.generated.js`;
const write = (label, source) => writeFile(directory + name(label), source);
const specifier = label => `./${name(label).slice("resources/".length)}`;
const load = label => import(`./${name(label)}`).then(module => module.value, error => `rejected: ${String(error).split("\n")[0].replace(/[^ ]*module-load-failed-before-evaluation[^ ]*/g, "<module>")}`);

globalThis.evaluations = [];
const evaluated = label => globalThis.evaluations.filter(entry => entry === label).length;

let outcome = "did not finish";
(async function () {
    // A module that a static import names does not exist, and exists for the next load.
    write("missing-importer", `import { value as imported } from "${specifier("missing")}"; globalThis.evaluations.push("missing-importer"); export const value = imported;`);
    shouldBe((await load("missing-importer")).startsWith("rejected: "), true, "importer of a missing module");
    write("missing", `globalThis.evaluations.push("missing"); export const value = 42;`);
    shouldBe(await load("missing-importer"), 42, "importer of a module that exists now");
    shouldBe(await load("missing-importer"), 42, "importer of a module that exists now, again");
    shouldBe(evaluated("missing-importer"), 1, "evaluations of the importer");
    shouldBe(evaluated("missing"), 1, "evaluations of the module that was missing");

    // The same through two importers: each of them is loaded again.
    write("deep-root", `export { value } from "${specifier("deep-middle")}";`);
    write("deep-middle", `export { value } from "${specifier("deep-leaf")}";`);
    shouldBe((await load("deep-root")).startsWith("rejected: "), true, "root above a missing module");
    shouldBe((await load("deep-middle")).startsWith("rejected: "), true, "importer of a missing module, on its own");
    write("deep-leaf", `export const value = 42;`);
    shouldBe(await load("deep-root"), 42, "root above a module that exists now");

    // A module that does not parse, and parses for the next load.
    write("syntax-importer", `export { value } from "${specifier("syntax")}";`);
    write("syntax", `export const value == 42;`);
    shouldBe((await load("syntax-importer")).startsWith("rejected: SyntaxError"), true, "importer of a module that does not parse");
    write("syntax", `export const value = 42;`);
    shouldBe(await load("syntax-importer"), 42, "importer of a module that parses now");

    // The module that failed is loaded again by a static import of another importer, and by a load of its own.
    write("second-importer-a", `export { value } from "${specifier("second")}";`);
    write("second", `export const value == 42;`);
    shouldBe((await load("second-importer-a")).startsWith("rejected: SyntaxError"), true, "first importer");
    write("second", `globalThis.evaluations.push("second"); export const value = 42;`);
    write("second-importer-b", `export { value } from "${specifier("second")}";`);
    shouldBe(await load("second-importer-b"), 42, "second importer");
    shouldBe(await load("second-importer-a"), 42, "first importer, again");
    shouldBe(await load("second"), 42, "the module itself");
    shouldBe(evaluated("second"), 1, "evaluations of the module both import");

    // The importer itself is fetched again: the import that failed is gone from it.
    write("rewritten", `export { value } from "${specifier("never-written")}";`);
    shouldBe((await load("rewritten")).startsWith("rejected: "), true, "module with an import of a missing module");
    write("rewritten", `export const value = 42;`);
    shouldBe(await load("rewritten"), 42, "the same module without that import");

    // A cycle whose modules another graph of the same load has taken up before the load failed.
    write("cycle-a", `import { b } from "${specifier("cycle-b")}"; globalThis.evaluations.push("cycle-a"); export const a = 40; export const value = a + b;`);
    write("cycle-b", `import "${specifier("cycle-a")}"; import { two } from "${specifier("cycle-leaf")}"; globalThis.evaluations.push("cycle-b"); export const b = two;`);
    shouldBe((await load("cycle-a")).startsWith("rejected: "), true, "cycle with a missing module");
    write("cycle-leaf", `export const two = 2;`);
    shouldBe(await load("cycle-a"), 42, "cycle with a module that exists now");
    shouldBe(evaluated("cycle-a"), 1, "evaluations of the cycle's first module");
    shouldBe(evaluated("cycle-b"), 1, "evaluations of the cycle's second module");

    // Two loads of one importer overlap. Both fail, and the next one loads.
    write("overlap", `export { value } from "${specifier("overlap-leaf")}";`);
    const overlapping = [load("overlap"), load("overlap")];
    shouldBe((await overlapping[0]).startsWith("rejected: "), true, "first of two overlapping loads");
    shouldBe((await overlapping[1]).startsWith("rejected: "), true, "second of two overlapping loads");
    write("overlap-leaf", `globalThis.evaluations.push("overlap-leaf"); export const value = 42;`);
    const again = [load("overlap"), load("overlap")];
    shouldBe(await again[0], 42, "first of two overlapping loads, again");
    shouldBe(await again[1], 42, "second of two overlapping loads, again");
    shouldBe(evaluated("overlap-leaf"), 1, "evaluations below the overlapping loads");

    // A module that threw while it was evaluated is not evaluated again, and neither is its importer.
    write("throws-importer", `import "${specifier("throws")}"; globalThis.evaluations.push("throws-importer"); export const value = 42;`);
    write("throws", `globalThis.evaluations.push("throws"); throw new Error("thrown by the module"); export const value = 1;`);
    shouldBe(await load("throws-importer"), "rejected: Error: thrown by the module", "importer of a module that throws");
    write("throws", `export const value = 42;`);
    shouldBe(await load("throws-importer"), "rejected: Error: thrown by the module", "importer of a module that threw");
    shouldBe(await load("throws"), "rejected: Error: thrown by the module", "module that threw");
    shouldBe(evaluated("throws"), 1, "evaluations of the module that threw");
    shouldBe(evaluated("throws-importer"), 0, "evaluations of its importer");

    // A module that did not link stays as it is: its modules are loaded, and one of them may have run elsewhere.
    write("link-importer", `import { absent } from "${specifier("link")}"; export const value = absent;`);
    write("link", `export const value = 42;`);
    const linkError = await load("link-importer");
    shouldBe(linkError.startsWith("rejected: SyntaxError"), true, "importer of a name that is not exported");
    write("link", `export const absent = 42;`);
    shouldBe(await load("link-importer"), linkError, "importer of a name that was not exported");

    const afterJobs = (count, callback) => {
        let job = Promise.resolve();
        while (count--)
            job = job.then(() => { });
        return job.then(callback);
    };

    // A second load by name starts at every step of the first one. At some of them the realm takes the module's record
    // from the first load, which then fails. The next load must not be answered with that record.
    for (let delay = 0; delay <= 7; ++delay) {
        const [twice, leaf] = [`twice-${delay}`, `twice-${delay}-leaf`];
        write(twice, `export { value } from "${specifier(leaf)}";`);
        const loads = [load(twice)];
        await afterJobs(delay, () => { loads.push(load(twice)); });
        for (const result of await Promise.all(loads))
            shouldBe(String(result).startsWith("rejected: "), true, `second load after ${delay} jobs: a load while the import is missing`);
        write(leaf, `globalThis.evaluations.push("${leaf}"); export const value = 42;`);
        shouldBe(await load(twice), 42, `second load after ${delay} jobs: the next load`);
        shouldBe(evaluated(leaf), 1, `second load after ${delay} jobs: evaluations of the import`);
    }

    // A graph takes up a record that another load made while that load is in flight. Here an importer reaches a module
    // at every step of that module's failing load, and the missing module appears at every step too. In some of these
    // orders the importer's graph fetches the missing module itself and is done loading with a record whose own load
    // has failed and left the registry. That graph has to fail as well. In every order, the next load of each module
    // succeeds, and no module runs twice.
    for (let importerDelay = 0; importerDelay <= 4; ++importerDelay) {
        for (let moduleDelay = 2; moduleDelay <= 7; ++moduleDelay) {
            const order = `${importerDelay}-${moduleDelay}`;
            const [importer, middle, leaf] = [`order-${order}-importer`, `order-${order}-middle`, `order-${order}-leaf`];
            write(importer, `import "${specifier(middle)}"; globalThis.evaluations.push("${importer}"); export const value = 42;`);
            write(middle, `import "${specifier(leaf)}"; globalThis.evaluations.push("${middle}"); export const value = 42;`);
            const loads = [load(middle)];
            const importerStarted = afterJobs(importerDelay, () => { loads.push(load(importer)); });
            const leafWritten = afterJobs(moduleDelay, () => { write(leaf, `globalThis.evaluations.push("${leaf}"); export const value = 42;`); });
            await importerStarted;
            await leafWritten;
            await Promise.all(loads);
            shouldBe(await load(importer), 42, `order ${order}: importer`);
            shouldBe(await load(middle), 42, `order ${order}: module with the missing import`);
            for (const label of [importer, middle, leaf])
                shouldBe(evaluated(label), 1, `order ${order}: evaluations of ${label}`);
        }
    }

    outcome = "passed";
}()).catch(error => { outcome = error; });

drainMicrotasks();
if (outcome !== "passed")
    throw outcome;
