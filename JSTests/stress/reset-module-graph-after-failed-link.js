// A Link() that fails keeps what it made on the module records that did not link: the next Link() of the same graph
// makes the same again. That only holds while [[LoadedModules]] does not change. A host that gives such a record
// another module for a request calls CyclicModuleRecord::resetGraphAfterFailedLink() first, which is what
// $vm.resetModuleGraphAfterFailedLink(error) does for the module whose Link() threw `error`.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${expected} but got ${actual}`);
}

// What the module records hold, not which cells are alive: the collector scans the stack conservatively.
function heldByModuleRecords() {
    const snapshot = generateHeapSnapshot();
    const classNames = new Map();
    for (let i = 0; i < snapshot.nodes.length; i += 4)
        classNames.set(snapshot.nodes[i], snapshot.nodeClassNames[snapshot.nodes[i + 2]]);
    const holders = { JSModuleEnvironment: new Set, ModuleNamespaceObject: new Set, ModuleProgramExecutable: new Set };
    for (let i = 0; i < snapshot.edges.length; i += 4) {
        if (classNames.get(snapshot.edges[i]) === "ModuleRecord")
            holders[classNames.get(snapshot.edges[i + 1])]?.add(snapshot.edges[i]);
    }
    return `${holders.JSModuleEnvironment.size} environments, ${holders.ModuleNamespaceObject.size} namespace objects, ${holders.ModuleProgramExecutable.size} executables`;
}

function link(name) {
    let outcome = "pending";
    import(`./resources/reset-module-graph-after-failed-link/${name}`).then(
        namespace => { outcome = namespace; },
        error => { outcome = error; });
    drainMicrotasks();
    return outcome;
}

shouldBe(heldByModuleRecords(), "0 environments, 0 namespace objects, 0 executables");

// a.js and b.js import each other. a.js, which imports the namespace of b.js, finishes InitializeEnvironment. Then
// b.js fails: c.js, which is linked by then, does not have the export that b.js imports.
let error = link("cycle/b.js");
shouldBe(error instanceof SyntaxError, true);
shouldBe(heldByModuleRecords(), "2 environments, 1 namespace objects, 3 executables");
shouldBe($vm.resetModuleGraphAfterFailedLink(error), true);
// c.js is linked: it keeps its environment.
shouldBe(heldByModuleRecords(), "1 environments, 0 namespace objects, 1 executables");

// The next Link() of the graph does InitializeEnvironment for a.js again, and fails in b.js as before.
error = link("cycle/root.js");
shouldBe(error instanceof SyntaxError, true);
shouldBe(heldByModuleRecords(), "2 environments, 1 namespace objects, 3 executables");
shouldBe($vm.resetModuleGraphAfterFailedLink(error), true);
shouldBe(heldByModuleRecords(), "1 environments, 0 namespace objects, 1 executables");

// Without the reset, the records keep what the failed Link() made.
error = link("cycle/root2.js");
shouldBe(error instanceof SyntaxError, true);
shouldBe(heldByModuleRecords(), "2 environments, 1 namespace objects, 3 executables");
shouldBe(link("cycle/root3.js") instanceof SyntaxError, true);
shouldBe(heldByModuleRecords(), "2 environments, 1 namespace objects, 3 executables");
shouldBe($vm.resetModuleGraphAfterFailedLink(error), true);
shouldBe(heldByModuleRecords(), "1 environments, 0 namespace objects, 1 executables");

// p.js and q.js import each other. q.js imports `ns`, which p.js exports as the namespace of s.js: s.js gets its
// namespace object before InnerModuleLinking has visited it. Then r.js fails, and s.js is never visited.
error = link("not-visited/p.js");
shouldBe(error instanceof SyntaxError, true);
shouldBe(heldByModuleRecords(), "3 environments, 1 namespace objects, 4 executables");
shouldBe($vm.resetModuleGraphAfterFailedLink(error), true);
shouldBe(heldByModuleRecords(), "2 environments, 0 namespace objects, 2 executables");

// The modules that linked are as they were.
shouldBe(link("cycle/c.js").y, 1);
shouldBe(link("not-visited/c.js").y, 1);

// Not the error of a Link().
shouldBe($vm.resetModuleGraphAfterFailedLink(new SyntaxError("not from a module")), false);
shouldBe($vm.resetModuleGraphAfterFailedLink(undefined), false);
