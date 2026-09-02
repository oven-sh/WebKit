//@ requireOptions("--useJSThreads=1")
// An indexing transition on a prototype fires the old structure's transition
// watchpoints. An adaptive watchpoint (here, the array iterator protocol set)
// re-installs itself on the object's structure when it fires. The fire must
// come after the new structure is published, or the re-install sees the old,
// already fired structure and invalidates the set. The set being invalid sends
// BigInt64Array.from(int32Array) to the iterator path, which throws a different
// TypeError.

const transitions = [
    "Object.defineProperty(Array.prototype, 100, { get() { return 1; } });",
    "Array.prototype[0] = 1;",
    "Array.prototype[0] = 1; Array.prototype[1] = 1.5;",
    "Array.prototype[0] = 1; Object.defineProperty(Array.prototype, 1, { get() { return 1; } });",
    "Object.prototype[0] = 1;",
    "Object.defineProperty(Object.prototype, 0, { get() { return 1; } });",
    "Object.getPrototypeOf([][Symbol.iterator]())[0] = 1;",
];

const expected = "TypeError: Content types of source and destination typed arrays are different";

for (const source of transitions) {
    const global = createGlobalObject();
    global.eval(source);
    let error = null;
    try {
        global.BigInt64Array.from(new global.Int32Array([0, 1]));
    } catch (e) {
        error = String(e);
    }
    if (error !== expected)
        throw new Error(`after ${source}: got ${error}`);
}
