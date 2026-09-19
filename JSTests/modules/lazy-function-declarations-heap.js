//@ requireOptions("--useLazyModuleFunctionDeclarations=1")
import * as lib from "./lazy-function-declarations/lib.js";
import { shouldBe } from "./resources/assert.js";

// Heap snapshots and collections while function declarations are uninstantiated, and while they get instantiated.
function snapshot() {
    let json = generateHeapSnapshot();
    shouldBe(Array.isArray(json.nodes), true);
    generateHeapSnapshotForGCDebugging();
    return json.nodes.length;
}
let before = snapshot();
gc();
let functions = [];
for (let name of Object.keys(lib)) {
    if (typeof lib[name] === "function" && !functions.includes(lib[name]))
        functions.push(lib[name]);
    if (functions.length % 5 === 0)
        gc();
}
edenGC();
let after = snapshot();
shouldBe(after > 0 && before > 0, true);
for (let name of Object.keys(lib)) {
    if (typeof lib[name] === "function")
        shouldBe(functions.includes(lib[name]), true);
}
fullGC();
shouldBe(lib.plain(1, 2), 3);
shouldBe(lib.callsPrivate(1), 3);
