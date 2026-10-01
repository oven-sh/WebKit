// A Symbol that is reachable only as a property key of a live object is live: a WeakMap entry
// keyed by it stays, a WeakRef to it is not cleared, and Object.getOwnPropertySymbols returns the
// same cell. A Structure holds the key as its SymbolImpl, so nothing else keeps the cell.

function shouldBe(actual, expected, message)
{
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}: ${message}`);
}

// Overwrites stack slots that could still point at a symbol, so that the conservative scan
// does not keep it alive by accident.
function scrub(depth)
{
    return depth > 0 ? scrub(depth - 1) + depth : 0;
}
noInline(scrub);

const registry = new WeakMap();
const weakSet = new WeakSet();

function addKey(object, attributes)
{
    const key = Symbol("key");
    registry.set(key, "value");
    weakSet.add(key);
    if (attributes === "define")
        Object.defineProperty(object, key, { value: true, enumerable: false, configurable: true, writable: true });
    else if (attributes === "accessor")
        Object.defineProperty(object, key, { get() { return true; }, configurable: true });
    else
        object[key] = true;
}
noInline(addKey);

function check(object, message)
{
    const keys = Object.getOwnPropertySymbols(object);
    const key = keys[keys.length - 1];
    shouldBe(registry.has(key), true, `WeakMap entry, ${message}`);
    shouldBe(weakSet.has(key), true, `WeakSet entry, ${message}`);
}

function makeDictionary()
{
    const object = {};
    for (let i = 0; i < 100; ++i)
        object[`p${i}`] = i;
    delete object.p0;
    return object;
}

function makeClassInstance()
{
    class C { }
    return new C();
}

function makeArray()
{
    return [1, 2, 3];
}

function makeFunction()
{
    return function() { };
}

// Three collections per case keep the file well inside the time budget; one would do to show
// the bug, since the unfixed engine loses the symbol on the first collection.
function run(name, collect, makeObject, attributes)
{
    for (let i = 0; i < 3; ++i) {
        const object = makeObject();
        addKey(object, attributes);
        scrub(100);
        collect();
        check(object, `${name} #${i}`);
    }
}

for (const [name, makeObject] of [["transition", () => ({})], ["dictionary", makeDictionary], ["class instance", makeClassInstance], ["array", makeArray], ["function", makeFunction]]) {
    for (const attributes of ["put", "define", "accessor"]) {
        run(`${name} ${attributes} full`, fullGC, makeObject, attributes);
        run(`${name} ${attributes} eden`, edenGC, makeObject, attributes);
    }
}

// A key added to an object that is already old.
{
    const old = makeDictionary();
    fullGC();
    run("old dictionary eden", edenGC, () => old, "put");
    run("old dictionary full", fullGC, () => old, "put");
    const oldPlain = {};
    fullGC();
    run("old object eden", edenGC, () => oldPlain, "put");
}

// A key on a prototype, reached through an instance.
{
    const proto = {};
    const instance = Object.create(proto);
    addKey(proto, "put");
    scrub(100);
    fullGC();
    check(Object.getPrototypeOf(instance), "prototype");
}

// The key comes back through Reflect.ownKeys (from oven-sh/WebKit#326).
{
    function makeMapWithPropertySymbol()
    {
        const map = new WeakMap();
        const symbol = Symbol("property");
        const object = { [symbol]: true };
        map.set(symbol, 42);
        return [map, object];
    }
    const [mapWithPropertySymbol, objectWithSymbolProperty] = makeMapWithPropertySymbol();
    scrub(100);
    gc();
    const propertySymbol = Reflect.ownKeys(objectWithSymbolProperty)[0];
    shouldBe(mapWithPropertySymbol.get(propertySymbol), 42, "Reflect.ownKeys");
}

// A WeakRef made in this job keeps its target until the job ends, so the WeakRef checks run
// in a later task.
const liveObject = {};
const liveRef = (() => {
    const key = Symbol("ref");
    liveObject[key] = 1;
    return new WeakRef(key);
})();

// A symbol that stops being a key is collectable: nothing holds it once the structure that has
// the key is gone.
const goneRef = (() => {
    const key = Symbol("gone");
    const object = { [key]: 1 };
    registry.set(key, "value");
    return new WeakRef(key);
})();

// The value of a WeakMap entry reaches the object that has the symbol as a key: the cycle is
// collected, as an ephemeron.
const cycleRef = (() => {
    const key = Symbol("cycle");
    const object = { [key]: 1 };
    registry.set(key, object);
    return new WeakRef(key);
})();

setTimeout(() => {
    scrub(100);
    fullGC();
    shouldBe(liveRef.deref(), Object.getOwnPropertySymbols(liveObject)[0], "WeakRef to a key");
    shouldBe(typeof goneRef.deref(), "undefined", "a symbol that is no longer a key is collectable");
    shouldBe(typeof cycleRef.deref(), "undefined", "ephemeron cycle through a WeakMap value");
}, 0);
